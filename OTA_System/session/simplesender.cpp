#include "simplesender.h"

#include "binsplitter.h"
#include "itransport.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>

extern "C" {
#include "ota_protocol.h"
}

namespace {

// sessionId==0("자동 생성" 신호)일 때 실제로 쓸 랜덤 session_id를 만듦.
// 0은 "자동 생성해달라"는 의미로 예약해뒀으므로, 혹시라도 난수가 0이 나오면
// 다시 뽑아서 실제 session_id로 0이 쓰이는 일이 없게 함(가능성은 극히 낮지만
// 방어적으로 처리).
uint32_t generateSessionId()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFFu);
    return dist(gen);
}

// 파일 크기만 필요할 때 쓰는 헬퍼 (BinSplitter::split 내부와 동일한 방식 —
// std::ios::ate로 열어서 tellg()로 바로 크기 확인).
bool readFileSize(const std::string &filePath, uint32_t *sizeOut, std::string *errorMessage)
{
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

} // namespace

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

    const int effectiveChunkSize =
        (chunkSize <= 0) ? static_cast<int>(OTA_MAX_PAYLOAD_SIZE) : chunkSize;

    uint32_t imageSize = 0;
    if (!readFileSize(filePath, &imageSize, &result.errorMessage))
        return result;

    const uint32_t totalChunks = ota_protocol_total_chunks(imageSize);
    const uint32_t effectiveSessionId = (sessionId == 0) ? generateSessionId() : sessionId;

    // ---------- 1. OTA_START ----------
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

    // ---------- 2. OTA_DATA (전부) ----------
    std::string splitError;
    const auto chunks =
        BinSplitter::split(filePath, effectiveSessionId, effectiveChunkSize, &splitError);
    if (chunks.empty()) {
        result.errorMessage = splitError.empty() ? "BinSplitter::split 실패" : splitError;
        return result;
    }
    if (chunks.size() != totalChunks) {
        // 이론상 안 일어나야 하지만, 나눗셈 로직이 어긋나면 OTA_START에 실은
        // total_chunks와 실제 보낼 청크 수가 달라져 수신측이 혼란스러워지므로
        // 방어적으로 확인.
        result.errorMessage = "청크 개수 불일치: OTA_START에는 " + std::to_string(totalChunks)
                               + "개로 알렸는데 실제로는 " + std::to_string(chunks.size()) + "개";
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

    // ---------- 3. OTA_END ----------
    ota_end_fields_t endFields{};
    endFields.session_id = effectiveSessionId;
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
    result.sessionId = effectiveSessionId;
    result.totalChunks = totalChunks;
    return result;
}
