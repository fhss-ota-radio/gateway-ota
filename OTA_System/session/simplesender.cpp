#include "simplesender.h"

#include "binsplitter.h"
#include "itransport.h"
#include "simplereceiver.h" // performHandshake()가 tryReceiveOnce()로 응답을 기다리는 데 씀

#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>

extern "C" {
#include "ota_protocol.h"
}

uint32_t generateSessionId()
{
    // 0은 "자동 생성해달라"는 의미로 예약해뒀으므로, 혹시라도 난수가 0이
    // 나오면 다시 뽑아서 실제 session_id로 0이 쓰이는 일이 없게 함(가능성은
    // 극히 낮지만 방어적으로 처리).
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFFu);
    return dist(gen);
}

bool readFileSize(const std::string &filePath, uint32_t *sizeOut, std::string *errorMessage)
{
    // BinSplitter::split 내부와 동일한 방식 — std::ios::ate로 열어서
    // tellg()로 바로 크기 확인.
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (errorMessage)
            *errorMessage = "파일을 열 수 없습니다: " + filePath;
        return false;
    }
    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0) {
        if (errorMessage)
            *errorMessage = "파일이 비어 있습니다: " + filePath;
        return false;
    }
    *sizeOut = static_cast<uint32_t>(fileSize);
    return true;
}

SimpleSendResult sendDataAndEnd(
    ITransport &transport,
    const std::string &filePath,
    uint32_t sessionId,
    uint32_t imageSize,
    uint32_t totalChunks,
    int chunkSize,
    int chunkDelayMs,
    const std::function<void(const SimpleSendProgress &)> &onProgress)
{
    SimpleSendResult result;

    if (!transport.isOpen()) {
        result.errorMessage = "transport가 열려 있지 않습니다 (open() 먼저 호출 필요)";
        return result;
    }

    const int effectiveChunkSize =
        (chunkSize <= 0) ? static_cast<int>(OTA_MAX_PAYLOAD_SIZE) : chunkSize;

    // ---------- 1. OTA_DATA (전부) ----------
    std::string splitError;
    const auto chunks = BinSplitter::split(filePath, sessionId, effectiveChunkSize, &splitError);
    if (chunks.empty()) {
        result.errorMessage = splitError.empty() ? "BinSplitter::split 실패" : splitError;
        return result;
    }
    if (chunks.size() != totalChunks) {
        // 이론상 안 일어나야 하지만, 나눗셈 로직이 어긋나면 OTA_START에 실은
        // total_chunks와 실제 보낼 청크 수가 달라져 수신측이 혼란스러워지므로
        // 방어적으로 확인.
        result.errorMessage = "청크 개수 불일치: 호출부는 " + std::to_string(totalChunks)
                               + "개로 알고 있는데 실제로는 " + std::to_string(chunks.size()) + "개";
        return result;
    }

    SimpleSendProgress progress;
    progress.totalChunks = totalChunks;

    for (const auto &chunk : chunks) {
        if (!transport.send(chunk.packet)) {
            result.errorMessage =
                "OTA_DATA 전송 실패 (sequence=" + std::to_string(chunk.header.sequence) + ")";
            return result;
        }
        ++progress.sentChunks;
        if (onProgress)
            onProgress(progress);
        if (chunkDelayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(chunkDelayMs));
    }

    // ---------- 2. OTA_END ----------
    ota_end_fields_t endFields{};
    endFields.session_id = sessionId;
    endFields.image_size = imageSize;
    endFields.total_chunks = totalChunks;

    uint8_t endPacket[OTA_END_PACKET_SIZE];
    const size_t endWritten = ota_protocol_encode_end(endPacket, sizeof(endPacket), &endFields);
    if (endWritten == 0) {
        result.errorMessage = "OTA_END 인코딩 실패";
        return result;
    }
    if (!transport.send({endPacket, endPacket + endWritten})) {
        result.errorMessage = "OTA_END 전송 실패 (transport.send)";
        return result;
    }

    result.success = true;
    result.sessionId = sessionId;
    result.totalChunks = totalChunks;
    return result;
}

SimpleSendResult simpleSendFile(
    ITransport &transport,
    const std::string &filePath,
    uint32_t targetDeviceId,
    uint32_t sessionId,
    int chunkSize,
    int chunkDelayMs,
    const std::function<void(const SimpleSendProgress &)> &onProgress)
{
    SimpleSendResult result;

    if (!transport.isOpen()) {
        result.errorMessage = "transport가 열려 있지 않습니다 (open() 먼저 호출 필요)";
        return result;
    }

    uint32_t imageSize = 0;
    if (!readFileSize(filePath, &imageSize, &result.errorMessage))
        return result;

    const uint32_t totalChunks = ota_protocol_total_chunks(imageSize);
    const uint32_t effectiveSessionId = (sessionId == 0) ? generateSessionId() : sessionId;

    // ---------- OTA_START ----------
    ota_start_fields_t startFields{};
    startFields.session_id = effectiveSessionId;
    startFields.target_device_id = targetDeviceId;
    startFields.image_size = imageSize;
    startFields.total_chunks = totalChunks;
    // image_sha256: 아직 계산 안 함 (simplesender.h 주석 참고 — 스모크테스트
    // 단계에서는 수신측도 검증하지 않으므로 의미 없는 값).
    std::memset(startFields.image_sha256, 0, sizeof(startFields.image_sha256));

    uint8_t startPacket[OTA_START_PACKET_SIZE];
    const size_t startWritten =
        ota_protocol_encode_start(startPacket, sizeof(startPacket), &startFields);
    if (startWritten == 0) {
        result.errorMessage = "OTA_START 인코딩 실패";
        return result;
    }
    if (!transport.send({startPacket, startPacket + startWritten})) {
        result.errorMessage = "OTA_START 전송 실패 (transport.send)";
        return result;
    }

    // ---------- DATA 전부 + END ----------
    // (핸드셰이크 없이 START 보내자마자 바로 이어서 쏨 — 이 함수가 원래부터
    // "단순 전송"인 이유. 응답 확인이 필요하면 performHandshake() +
    // sendDataAndEnd() 조합을 대신 쓸 것)
    return sendDataAndEnd(transport, filePath, effectiveSessionId, imageSize, totalChunks,
                           chunkSize, chunkDelayMs, onProgress);
}

// ============================================================================
// 핸드셰이크 (simplesender.h 상단 설명 참고 — 별도 파일로 뺐다가 파일 개수를
// 늘리지 않기 위해 다시 여기로 합침)
// ============================================================================

HandshakeResult performHandshake(
    ITransport &transport,
    const std::string &filePath,
    uint32_t targetDeviceId,
    uint32_t sessionId,
    int timeoutMs,
    int maxRetry)
{
    HandshakeResult result;

    if (!transport.isOpen()) {
        result.errorMessage = "transport가 열려 있지 않습니다 (open() 먼저 호출 필요)";
        return result;
    }

    uint32_t imageSize = 0;
    if (!readFileSize(filePath, &imageSize, &result.errorMessage))
        return result;

    const uint32_t totalChunks = ota_protocol_total_chunks(imageSize);
    const uint32_t effectiveSessionId = (sessionId == 0) ? generateSessionId() : sessionId;

    ota_start_fields_t startFields{};
    startFields.session_id = effectiveSessionId;
    startFields.target_device_id = targetDeviceId;
    startFields.image_size = imageSize;
    startFields.total_chunks = totalChunks;
    std::memset(startFields.image_sha256, 0, sizeof(startFields.image_sha256));

    uint8_t startPacket[OTA_START_PACKET_SIZE];
    const size_t startWritten =
        ota_protocol_encode_start(startPacket, sizeof(startPacket), &startFields);
    if (startWritten == 0) {
        result.errorMessage = "OTA_START 인코딩 실패";
        return result;
    }

    for (int attempt = 1; attempt <= maxRetry; ++attempt) {
        if (!transport.send({startPacket, startPacket + startWritten})) {
            result.errorMessage = "OTA_START 전송 실패 (transport.send)";
            return result;
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        while (std::chrono::steady_clock::now() < deadline) {
            const auto packet = tryReceiveOnce(transport);

            // START/END에 대한 응답은 특정 청크가 아니라 "제어 패킷 자체에
            // 대한 응답"이라 sequence 자리에 OTA_CONTROL_SEQUENCE가 옴
            // (session/simplereceiver.cpp의 sendAckFor()도 그렇게 채워서
            // 보냄) — 이 값과 session_id가 둘 다 맞아야 "우리 START에 대한
            // 응답"이라고 확신할 수 있음.
            const bool matchesOurStart =
                packet.sessionId == effectiveSessionId && packet.sequence == OTA_CONTROL_SEQUENCE;

            if (packet.kind == ReceivedPacketKind::Ack && matchesOurStart) {
                result.success = true;
                result.sessionId = effectiveSessionId;
                result.imageSize = imageSize;
                result.totalChunks = totalChunks;
                return result;
            }

            if (packet.kind == ReceivedPacketKind::Nack && matchesOurStart) {
                // 상대가 START 자체를 거부함(result_code 참고 — 지금은 왜
                // 거부했는지 판단하는 로직이 없어서 타임아웃과 동일하게
                // 취급하고 다음 시도로 넘어감. NACK 사유별 처리는 OtaSession
                // 몫으로 남겨둠.
                break;
            }

            if (packet.kind == ReceivedPacketKind::Unknown && packet.raw.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            // 그 외(우리 START와 상관없는 다른 패킷)는 무시하고 계속 들음
        }
        // 이번 시도에서 응답 못 받음 -> 다음 attempt에서 START 재전송
    }

    result.errorMessage =
        "핸드셰이크 실패 — " + std::to_string(maxRetry) + "회 재시도 후에도 응답 없음";
    return result;
}
