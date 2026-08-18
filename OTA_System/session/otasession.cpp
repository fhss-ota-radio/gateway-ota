#include "otasession.h"

#include "itransport.h"
#include "simplereceiver.h" // tryReceiveOnce(), ReceivedPacketKind
#include "simplesender.h"   // generateSessionId(), readFileSize()

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

extern "C" {
#include "ota_protocol.h"
}

int64_t otaSessionNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char *otaSessionStateName(OtaSessionState state)
{
    switch (state) {
    case OtaSessionState::Idle:            return "Idle";
    case OtaSessionState::Handshaking:     return "Handshaking";
    case OtaSessionState::SendingBatch:    return "SendingBatch";
    case OtaSessionState::WaitingBatchAck: return "WaitingBatchAck";
    case OtaSessionState::Retransmitting:  return "Retransmitting";
    case OtaSessionState::WaitingEndAck:   return "WaitingEndAck";
    case OtaSessionState::Paused:          return "Paused";
    case OtaSessionState::Completed:       return "Completed";
    case OtaSessionState::Failed:          return "Failed";
    }
    return "Unknown";
}

OtaSession::OtaSession(ITransport &transport, int batchSize, int timeoutMs, int maxRetry,
                        int chunkDelayMs)
    : m_transport(transport)
    , m_batchSize(std::max(1, batchSize))
    , m_timeoutMs(std::max(1, timeoutMs))
    , m_maxRetry(std::max(0, maxRetry))
    , m_chunkDelayMs(std::max(0, chunkDelayMs))
{
}

void OtaSession::setState(OtaSessionState s)
{
    m_state = s;
    if (m_onStateChanged)
        m_onStateChanged(s);
}

void OtaSession::fail(const std::string &reason)
{
    m_errorMessage = reason;
    setState(OtaSessionState::Failed);
}

// ============================================================================
// start() / HANDSHAKING
// ============================================================================

bool OtaSession::start(const std::string &filePath, uint32_t targetDeviceId, uint32_t sessionId,
                        int64_t nowMs)
{
    if (m_state != OtaSessionState::Idle) {
        m_errorMessage =
            "이미 진행 중인 세션이 있습니다 (state=" + std::string(otaSessionStateName(m_state)) + ")";
        return false;
    }
    if (!m_transport.isOpen()) {
        m_errorMessage = "transport가 열려 있지 않습니다 (open() 먼저 호출 필요)";
        return false;
    }

    uint32_t imageSize = 0;
    if (!readFileSize(filePath, &imageSize, &m_errorMessage))
        return false;

    const uint32_t effectiveSessionId = (sessionId == 0) ? generateSessionId() : sessionId;

    std::string splitError;
    auto chunks = BinSplitter::split(filePath, effectiveSessionId,
                                      static_cast<int>(OTA_MAX_PAYLOAD_SIZE), &splitError);
    if (chunks.empty()) {
        m_errorMessage = splitError.empty() ? "BinSplitter::split 실패" : splitError;
        return false;
    }

    m_targetDeviceId = targetDeviceId;
    m_sessionId = effectiveSessionId;
    m_imageSize = imageSize;
    m_chunks = std::move(chunks);
    m_nextUnsentIndex = 0;
    m_batch.clear();
    m_totalAcked = 0;
    m_currentBatchNumber = 0;
    m_errorMessage.clear();

    enterHandshaking(nowMs < 0 ? otaSessionNowMs() : nowMs);
    return true;
}

void OtaSession::sendStartPacket()
{
    ota_start_fields_t fields{};
    fields.session_id = m_sessionId;
    fields.target_device_id = m_targetDeviceId;
    fields.image_size = m_imageSize;
    fields.total_chunks = static_cast<uint32_t>(m_chunks.size());
    // image_sha256: 아직 계산 안 함 — simplesender.h와 같은 이유(수신측도 아직
    // 검증 안 함). SHA256 무결성 검증은 별도 작업으로 남겨둠.
    std::memset(fields.image_sha256, 0, sizeof(fields.image_sha256));

    uint8_t packet[OTA_START_PACKET_SIZE];
    const size_t written = ota_protocol_encode_start(packet, sizeof(packet), &fields);
    if (written == 0) {
        fail("OTA_START 인코딩 실패");
        return;
    }
    m_transport.send(std::vector<uint8_t>(packet, packet + written));
}

void OtaSession::enterHandshaking(int64_t nowMs)
{
    setState(OtaSessionState::Handshaking);
    m_controlRetryCount = 0;
    sendStartPacket();
    m_controlSentAtMs = nowMs;
}

void OtaSession::tickHandshaking(int64_t nowMs)
{
    const auto packet = tryReceiveOnce(m_transport);

    // START/END에 대한 응답은 "제어 패킷 자체에 대한 응답"이라 sequence 자리에
    // OTA_CONTROL_SEQUENCE가 옴 (simplesender.cpp performHandshake()와 동일 규칙).
    const bool matchesOurStart =
        packet.sessionId == m_sessionId && packet.sequence == OTA_CONTROL_SEQUENCE;

    if (packet.kind == ReceivedPacketKind::Ack && matchesOurStart) {
        enterSendingBatch(nowMs);
        return;
    }
    if (packet.kind == ReceivedPacketKind::Nack && matchesOurStart) {
        // 상대가 START 자체를 거부함 — 왜 거부했는지 판단하는 로직은 아직 없어서
        // (result_code별 분기는 향후 과제) 재시도로 취급.
        if (++m_controlRetryCount > m_maxRetry) {
            fail("핸드셰이크 실패 — NACK, " + std::to_string(m_maxRetry) + "회 재시도 초과");
            return;
        }
        sendStartPacket();
        m_controlSentAtMs = nowMs;
        return;
    }
    if (nowMs - m_controlSentAtMs > m_timeoutMs) {
        if (++m_controlRetryCount > m_maxRetry) {
            fail("핸드셰이크 실패 — " + std::to_string(m_maxRetry) + "회 재시도 후에도 응답 없음");
            return;
        }
        sendStartPacket();
        m_controlSentAtMs = nowMs;
    }
}

// ============================================================================
// SENDING_BATCH / WAITING_BATCH_ACK / RETRANSMITTING
// (docs/fsm-design.md §6 "재전송 알고리즘" 그대로 구현)
// ============================================================================

void OtaSession::enterSendingBatch(int64_t nowMs)
{
    setState(OtaSessionState::SendingBatch);

    const uint32_t remaining = static_cast<uint32_t>(m_chunks.size()) - m_nextUnsentIndex;
    const uint32_t count = std::min<uint32_t>(remaining, static_cast<uint32_t>(m_batchSize));

    m_batch.clear();
    m_batch.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto &chunk = m_chunks[m_nextUnsentIndex + i];
        BatchSlot slot;
        slot.sequence = chunk.header.sequence;
        slot.packet = chunk.packet;
        m_batch.push_back(std::move(slot));
    }
    m_nextUnsentIndex += count;
    ++m_currentBatchNumber;

    // 배치 안 청크를 처음부터 끝까지 순서대로 전부 전송 — 슬라이딩 윈도우가
    // 아니라 고정 크기 배치를 한꺼번에 다 쏘는 방식 (fsm-design.md §6).
    // 중간에 개별 ACK를 기다리지 않음: 확인은 아래 WAITING_BATCH_ACK에서 함.
    for (auto &slot : m_batch) {
        if (!m_transport.send(slot.packet)) {
            fail("OTA_DATA 전송 실패 (sequence=" + std::to_string(slot.sequence) + ")");
            return;
        }
        slot.sentAtMs = nowMs;
        if (m_chunkDelayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(m_chunkDelayMs));
    }

    setState(OtaSessionState::WaitingBatchAck);
}

bool OtaSession::retransmitSlot(BatchSlot &slot, int64_t nowMs)
{
    ++slot.retryCount;
    if (slot.retryCount > m_maxRetry) {
        fail("배치 재전송 한도(" + std::to_string(m_maxRetry) + "회) 초과 (sequence=" +
             std::to_string(slot.sequence) + ")");
        return false;
    }
    // RETRANSMITTING은 이 슬롯 하나 재전송하는 동안만 순간적으로 거쳐감
    // (fsm-design.md 상태 다이어그램: RETRANSMITTING -> WAITING_BATCH_ACK).
    setState(OtaSessionState::Retransmitting);
    m_transport.send(slot.packet);
    slot.sentAtMs = nowMs;
    setState(OtaSessionState::WaitingBatchAck);
    return true;
}

void OtaSession::tickWaitingBatchAck(int64_t nowMs)
{
    // 1. 수신 확인 (논블로킹 폴링 — 한 틱에 패킷 하나만 처리, tryReceiveOnce와 동일 패턴)
    const auto packet = tryReceiveOnce(m_transport);
    if ((packet.kind == ReceivedPacketKind::Ack || packet.kind == ReceivedPacketKind::Nack)
        && packet.sessionId == m_sessionId) {
        for (auto &slot : m_batch) {
            if (slot.acked || slot.sequence != packet.sequence)
                continue;

            const bool ok = (packet.kind == ReceivedPacketKind::Ack
                              && packet.resultCode == static_cast<uint8_t>(OTA_RESULT_OK));
            if (ok) {
                slot.acked = true;
                ++m_totalAcked;
            } else {
                // NACK 이중 방어 — 타임아웃을 기다리지 않고 그 슬롯만 즉시 재전송
                // (fsm-design.md §6 "슬롯별 독립 타이머 + NACK 이중 방어")
                if (!retransmitSlot(slot, nowMs))
                    return; // fail() 처리됨
            }
            break;
        }
    }

    // 2. 타임아웃 확인 — 아직 acked=false인 슬롯만
    for (auto &slot : m_batch) {
        if (slot.acked)
            continue;
        if (nowMs - slot.sentAtMs <= m_timeoutMs)
            continue;
        if (!retransmitSlot(slot, nowMs))
            return; // fail() 처리됨
    }

    // 3. 배치 전체 완료 확인 — 슬롯 하나가 끝났다고 다음 seq를 채워 넣지 않고,
    // 배치 전체가 acked 될 때까지 이 배치 안에서만 재시도 (슬라이딩 윈도우와의 핵심 차이).
    const bool batchDone =
        std::all_of(m_batch.begin(), m_batch.end(), [](const BatchSlot &s) { return s.acked; });
    if (!batchDone)
        return;

    if (m_nextUnsentIndex >= m_chunks.size())
        enterWaitingEndAck(nowMs);
    else
        enterSendingBatch(nowMs);
}

// ============================================================================
// WAITING_END_ACK
// ============================================================================

void OtaSession::sendEndPacket()
{
    ota_end_fields_t fields{};
    fields.session_id = m_sessionId;
    fields.image_size = m_imageSize;
    fields.total_chunks = static_cast<uint32_t>(m_chunks.size());

    uint8_t packet[OTA_END_PACKET_SIZE];
    const size_t written = ota_protocol_encode_end(packet, sizeof(packet), &fields);
    if (written == 0) {
        fail("OTA_END 인코딩 실패");
        return;
    }
    m_transport.send(std::vector<uint8_t>(packet, packet + written));
}

void OtaSession::enterWaitingEndAck(int64_t nowMs)
{
    setState(OtaSessionState::WaitingEndAck);
    m_controlRetryCount = 0;
    sendEndPacket();
    m_controlSentAtMs = nowMs;
}

void OtaSession::tickWaitingEndAck(int64_t nowMs)
{
    const auto packet = tryReceiveOnce(m_transport);
    const bool matchesOurEnd =
        packet.sessionId == m_sessionId && packet.sequence == OTA_CONTROL_SEQUENCE;

    if (packet.kind == ReceivedPacketKind::Ack && matchesOurEnd) {
        setState(OtaSessionState::Completed);
        return;
    }
    if (packet.kind == ReceivedPacketKind::Nack && matchesOurEnd) {
        // 재전송으로 해결되는 문제가 아님(대부분 최종 SHA256 불일치 같은 손상) —
        // 재시도하지 않고 바로 FAILED (fsm-design.md "왜 EV_END_NACK은 재시도가
        // 아니라 바로 FAILED인가" 참고).
        fail("OTA_END NACK 수신 (result_code=" + std::to_string(static_cast<int>(packet.resultCode))
             + ")");
        return;
    }
    if (nowMs - m_controlSentAtMs > m_timeoutMs) {
        if (++m_controlRetryCount > m_maxRetry) {
            fail("OTA_END 실패 — " + std::to_string(m_maxRetry) + "회 재시도 후에도 응답 없음");
            return;
        }
        sendEndPacket();
        m_controlSentAtMs = nowMs;
    }
}

// ============================================================================
// tick() 디스패처 / pause() / resume() / progress()
// ============================================================================

void OtaSession::tick(int64_t nowMs)
{
    switch (m_state) {
    case OtaSessionState::Handshaking:
        tickHandshaking(nowMs);
        break;
    case OtaSessionState::WaitingBatchAck:
        tickWaitingBatchAck(nowMs);
        break;
    case OtaSessionState::WaitingEndAck:
        tickWaitingEndAck(nowMs);
        break;
    default:
        // Idle: 아직 시작 안 함 / SendingBatch·Retransmitting: enterXxx() 안에서
        // 이미 동기적으로 처리되고 바로 다음 상태로 넘어가므로 tick에서 할 일 없음 /
        // Paused·Completed·Failed: 종료/보류 상태, tick 무시.
        break;
    }
}

void OtaSession::pause(int64_t nowMs)
{
    (void)nowMs;
    switch (m_state) {
    case OtaSessionState::SendingBatch:
    case OtaSessionState::WaitingBatchAck:
    case OtaSessionState::Retransmitting:
    case OtaSessionState::WaitingEndAck:
        m_pausedFrom = m_state;
        setState(OtaSessionState::Paused);
        break;
    default:
        break; // 그 외 상태에서는 조용히 무시
    }
}

void OtaSession::resume(int64_t nowMs)
{
    if (m_state != OtaSessionState::Paused)
        return;

    // 멈춰 있던 동안 흐른 시간만큼 모든 타이머를 지금 시각 기준으로 다시 맞춤 —
    // 안 하면 재개하자마자 오래 멈춰 있던 슬롯들이 전부 타임아웃으로 잡혀
    // 불필요한 재전송이 한꺼번에 발생함.
    m_controlSentAtMs = nowMs;
    for (auto &slot : m_batch)
        if (!slot.acked)
            slot.sentAtMs = nowMs;

    setState(m_pausedFrom);
}

OtaSessionProgress OtaSession::progress() const
{
    OtaSessionProgress p;
    p.totalChunks = static_cast<uint32_t>(m_chunks.size());
    p.totalBatches = (p.totalChunks == 0)
        ? 0
        : (p.totalChunks + static_cast<uint32_t>(m_batchSize) - 1)
              / static_cast<uint32_t>(m_batchSize);
    p.ackedChunks = m_totalAcked;
    p.currentBatchNumber = m_currentBatchNumber;
    return p;
}
