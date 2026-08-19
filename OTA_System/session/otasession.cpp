#include "otasession.h"

#include "itransport.h"
#include "sha256.h"          // sha256File() — image_sha256 필드 채우기용
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

    // OTA_START에 실어 보낼 실제 SHA256을 여기서 미리 계산해 둠 (파일을 한 번
    // 더 훑는 비용이 들지만, BinSplitter::split()도 어차피 파일 전체를 한 번
    // 읽으므로 두 번째 순회가 크게 비싸진 않음). ESP32(firmware-esp32의
    // ota_writer_finish())가 OTA_END에서 이 값을 진짜로 검증하기 때문에 0으로
    // 채워 보내면 데이터 전송이 전부 성공해도 마지막에 항상 거절당한다
    // (2026-08-19 발견, docs/roadmap.md 참고). 라즈베리파이끼리 테스트할 때는
    // 수신측이 이 값을 안 보므로 이전처럼 문제없이 동작한다.
    if (!sha256File(filePath, m_imageSha256, &m_errorMessage))
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
    // start()에서 sha256File()로 미리 계산해 둔 실제 해시를 그대로 실어 보냄
    // (ESP32 수신측 ota_writer_finish()가 OTA_END에서 이 값과 실제 수신된
    // 이미지의 해시를 비교·검증함 — 위 start() 주석 참고).
    static_assert(sizeof(fields.image_sha256) == sizeof(m_imageSha256),
                  "ota_start_fields_t.image_sha256 크기가 Sha256 출력(32byte)과 다릅니다");
    std::memcpy(fields.image_sha256, m_imageSha256, sizeof(fields.image_sha256));

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
    // 중간에 개별 ACK를 "기다리지"는 않지만(그러면 배치 방식 자체가 무의미),
    // 아래처럼 짧은 간격으로 폴링은 한다 — 이유는 바로 아래 주석.
    for (auto &slot : m_batch) {
        if (!m_transport.send(slot.packet)) {
            fail("OTA_DATA 전송 실패 (sequence=" + std::to_string(slot.sequence) + ")");
            return;
        }
        slot.sentAtMs = nowMs;

        // [효율 개선 2026-08-19] 예전에는 여기서 그냥 sleep_for(chunkDelayMs)만
        // 하고 recv()를 전혀 안 불렀다. 그런데 수신측은 DATA를 받자마자 바로
        // ACK를 보내므로, 배치 나머지를 마저 보내는 이 ~(batchSize-1)*
        // chunkDelayMs(예: 4*40=160ms) 구간 동안 이미 도착해 있을 ACK를
        // 소프트웨어가 아예 읽지 않고 흘려보내는 셈이었다. CC1101 자체는
        // 각 send() 완료 뒤 MCSM1 설정대로 RX로 자동 복귀하므로(반이중이라
        // "송신하는 그 순간"만 못 듣는 것) 이 sleep 구간에는 원래 들을 수
        // 있었는데, 코드가 안 듣고 있었을 뿐이다.
        //
        // 실기기 재검증(2026-08-19, docs/note/design-notes-gateway-ota-es.md
        // 27절)에서 이게 실제로 중복 수신의 주 원인으로 확인됐다 — 1067개
        // 청크 중 1062~1063개가 중복 수신됐는데, 정확히 "배치 최초 전송분은
        // 전부 놓치고 그 다음 타임아웃 재전송에서만 ACK를 받는" 패턴과
        // 맞아떨어진다. chunkDelayMs를 짧은 간격(kPollIntervalMs)으로
        // 쪼개서 그 사이사이 recv()를 폴링하면, 이미 도착한 ACK를 놓치지
        // 않고 바로 반영해서 불필요한 재전송을 줄일 수 있다.
        if (m_chunkDelayMs > 0) {
            constexpr int kPollIntervalMs = 5;
            int remainingMs = m_chunkDelayMs;
            while (remainingMs > 0) {
                const int step = std::min(remainingMs, kPollIntervalMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(step));
                remainingMs -= step;
                if (!pollAndApplyAckOrNack(nowMs))
                    return; // fail() 처리됨 — 배치 나머지 전송 중단
            }
        }
    }

    // [2026-08-19 추가했다가 같은 날 되돌림] 배치 전송 직후 flushRx()를
    // 불러서 "혹시 쌓인 RX 버퍼가 문제가 아닐까" 시도했었는데, 실기기
    // 재검증 결과 오히려 역효과였습니다 — 배치 안 첫 번째로 보낸 청크는
    // 배치 나머지를 다 보내는 동안(최대 (batchSize-1)*chunkDelayMs, 예:
    // 4*40=160ms) 이미 ACK가 도착해 RX 버퍼에 들어와 있을 가능성이 높은데,
    // 여기서 flushRx()를 부르면 그 "이미 도착한 진짜 ACK"까지 같이
    // 지워버려서 불필요한 재전송을 유발했습니다(2026-08-19 재검증 로그:
    // 수신측은 DATA를 끊김 없이 계속 받는데 송신측만 seq=0 재전송을
    // 반복하다 실패). RXFIFO_OVERFLOW 자체를 marc_state로 직접 확인하지
    // 않고 가설만으로 패치했던 게 원인 — 다음에 이 계열 문제가 다시
    // 나오면 cc1101_diag로 marc_state부터 확인하고 나서 손댈 것.
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

    // [추가 2026-08-19] 재전송에도 배치 전송과 같은 간격을 둔다.
    // enterSendingBatch()는 청크 사이에 chunkDelayMs를 넣는데 여기만 빠져
    // 있었다 — 그래서 재전송이 쉬는 시간 없이 붙어 나갔고, 반이중인 CC1101이
    // 그동안 돌아온 ACK를 못 들었다(위 tickWaitingBatchAck() 주석 참고).
    if (m_chunkDelayMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(m_chunkDelayMs));

    setState(OtaSessionState::WaitingBatchAck);
    return true;
}

bool OtaSession::pollAndApplyAckOrNack(int64_t nowMs)
{
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
                    return false; // fail() 처리됨
            }
            break;
        }
    }
    return true;
}

void OtaSession::tickWaitingBatchAck(int64_t nowMs)
{
    // 1. 수신 확인 (논블로킹 폴링 — 한 틱에 패킷 하나만 처리, tryReceiveOnce와 동일 패턴)
    if (!pollAndApplyAckOrNack(nowMs))
        return; // fail() 처리됨

    // 2. 타임아웃 확인 — 아직 acked=false인 슬롯만.
    //
    // [수정 2026-08-19] 한 틱에 "하나만" 재전송한다 (원래는 타임아웃된 슬롯
    // 전부를 이 루프에서 연달아 쐈음).
    //
    // 실기기 검증에서 드러난 문제: 배치 5개가 거의 동시에 타임아웃되면 이
    // 루프가 한 틱 안에서 5개를 쉬는 시간 없이 연속 송신했다. CC1101은
    // 반이중(half-duplex, 송신 중에는 수신 불가)이라, 그동안 수신측이 보낸
    // ACK가 전부 송신측 귀에 안 들어온다. 그러면 다음 타임아웃에 또 5개를
    // 몰아 쏘고 또 못 듣고… 이 악순환이 maxRetry회 반복되면 "재전송 한도
    // 초과"로 죽는다. 실제로 수신측 로그에는 해당 seq를 정상 수신하고 ACK를
    // 보낸 기록이 남아 있는데도 송신측만 못 받고 실패했다.
    //
    // 하나씩 보내면 send() 이후 다음 틱까지(호출부 기준 10ms) 반드시 수신
    // 기회가 생기고, 아래 chunkDelayMs 간격까지 더해져 상대 ACK가 돌아올
    // 시간이 확보된다.
    for (auto &slot : m_batch) {
        if (slot.acked)
            continue;
        if (nowMs - slot.sentAtMs <= m_timeoutMs)
            continue;
        if (!retransmitSlot(slot, nowMs))
            return; // fail() 처리됨
        break;      // 한 틱에 하나만 — 나머지는 다음 틱에서 다시 판정
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
