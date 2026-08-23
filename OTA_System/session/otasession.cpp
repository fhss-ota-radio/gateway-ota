#include "otasession.h"

#include "itransport.h"
#include "sha256.h"          // sha256File() — image_sha256 필드 채우기용
#include "simplereceiver.h" // tryReceiveOnce(), ReceivedPacketKind
#include "simplesender.h"   // generateSessionId(), readFileSize()

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" {
#include "ota_protocol.h"
}

namespace {
// 로그 메시지에 session_id를 사람이 읽기 좋은 8자리 16진수로 찍기 위한 헬퍼
// (2026-08-20, "Gateway ACK/NACK 로그 추가" 대응).
std::string toHex(uint32_t value)
{
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08X", value);
    return std::string(buf);
}
} // namespace

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
    log("FAIL: " + reason);
    setState(OtaSessionState::Failed);
}

// [2026-08-20 추가, "Gateway ACK/NACK 로그 추가" 요청 대응] m_onLog가 설정돼
// 있으면 그대로 전달, 없으면 조용히 무시. ACK/NACK 수신·재전송·타임아웃처럼
// StateCallback(상태 전환)만으로는 안 보이는 "WaitingBatchAck 안에서 무슨
// 일이 있었는지"를 실기기 테스트 중 바로 확인하기 위한 진단용 훅.
void OtaSession::log(const std::string &msg) const
{
    if (m_onLog)
        m_onLog(msg);
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
    // [2026-08-23 정정] 실기기로 확인해보니 단순 로그만으로는 부족했다 —
    // FHSS 호핑 중 -EBUSY 충돌이 실제로 재현됐고(design-notes 42절 6차
    // 시도), 배치 DATA 전송(enterSendingBatch())처럼 한 번 실패로 바로
    // 세션을 죽이면 호핑 환경에서는 거의 항상 실패한다. sendWithRetry()로
    // 짧게 재시도하도록 바꿈.
    (void)sendWithRetry(std::vector<uint8_t>(packet, packet + written), "OTA_START");
}

bool OtaSession::sendWithRetry(const std::vector<uint8_t> &packet, const std::string &context)
{
    // [2026-08-23 추가] otasession.h의 선언부 주석 참고 — FHSS 호핑 중
    // hop_worker의 자체 SYNC 송신과 겹치면 -EBUSY로 실패할 수 있는데,
    // 실제로는 몇 ms짜리 찰나(SYNC 패킷 13바이트 전송 시간 수준)라
    // 짧게 쉬었다 재시도하면 대부분 풀린다. kMaxAttempts/kRetryDelayMs는
    // 아직 실기기로 정밀 튜닝한 값이 아니라 "합리적인 기본값" — 다음
    // 실기기 테스트에서 재시도 로그 빈도를 보고 조정할 것.
    constexpr int kMaxAttempts = 4;
    constexpr int kRetryDelayMs = 8;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (m_transport.send(packet))
            return true;
        if (attempt < kMaxAttempts) {
            log(context + " transport.send() 실패(아마 -EBUSY, FHSS 호핑과 충돌) — "
                + std::to_string(kRetryDelayMs) + "ms 후 재시도 (" + std::to_string(attempt)
                + "/" + std::to_string(kMaxAttempts) + ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
        }
    }
    log(context + " transport.send() 실패 — " + std::to_string(kMaxAttempts)
        + "회 재시도 후에도 실패");
    return false;
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
    //
    // [버그 수정 2026-08-20, ESP32 담당자 리포트 클레임 1] acknowledged_type도
    // 같이 확인한다. 예전엔 sessionId+sequence만 봤는데, OTA_CONTROL_SEQUENCE가
    // START/END 응답 둘 다에 쓰이는 값이라(ota_protocol.h 규칙) 이론상 END에
    // 대한 응답을 START 응답으로 잘못 받아들일 여지가 있었다(sequence 공간이
    // 완전히 안 나뉘어 있었던 지점). 실제로 재현된 적은 없지만(START/END는
    // 시간상 겹칠 일이 없어서), 프로토콜이 명시적으로 실어 보내는 정보를
    // 검증 안 하고 버리는 건 잠재 버그라 이번에 같이 잠근다.
    const bool matchesOurStart =
        packet.sessionId == m_sessionId && packet.sequence == OTA_CONTROL_SEQUENCE
        && packet.acknowledgedType == static_cast<uint8_t>(OTA_PKT_START);

    if (packet.kind == ReceivedPacketKind::Ack && matchesOurStart) {
        log("START ACK 수신 (session_id=0x" + toHex(m_sessionId) + ") -> 배치 전송 시작");
        // 하드웨어 FLUSH_RX ioctl은 새 드라이버에서 이후 poll/read 통지를
        // 끊어 ACK가 RF로 도착해도 userspace가 못 읽게 했다. 수신 상태는
        // 건드리지 않고, START ACK 전에 누적된 응답만 bounded drain한다.
        constexpr int kMaxPreDataDrain = 64;
        int drained = 0;
        for (; drained < kMaxPreDataDrain; ++drained) {
            const auto stale = m_transport.recv();
            if (stale.empty())
                break;
        }
        log("START ACK 이후 pre-DATA stale RX userspace drain="
            + std::to_string(drained));
        enterSendingBatch(nowMs);
        return;
    }
    if (packet.kind == ReceivedPacketKind::Nack && matchesOurStart) {
        // 상대가 START 자체를 거부함 — 왜 거부했는지 판단하는 로직은 아직 없어서
        // (result_code별 분기는 향후 과제) 재시도로 취급.
        log("START NACK 수신 (result_code=" + std::to_string(static_cast<int>(packet.resultCode))
            + ")");
        if (++m_controlRetryCount > m_maxRetry) {
            fail("핸드셰이크 실패 — NACK, " + std::to_string(m_maxRetry) + "회 재시도 초과");
            return;
        }
        sendStartPacket();
        m_controlSentAtMs = nowMs;
        return;
    }
    if (nowMs - m_controlSentAtMs > m_timeoutMs) {
        log("START 응답 타임아웃 (재시도 " + std::to_string(m_controlRetryCount + 1) + "/"
            + std::to_string(m_maxRetry) + ")");
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

    if (count > 0) {
        log("배치 " + std::to_string(m_currentBatchNumber) + " 전송 시작 (seq="
            + std::to_string(m_batch.front().sequence) + ".."
            + std::to_string(m_batch.back().sequence) + ", " + std::to_string(count) + "개)");
    }

    // 배치 안 청크를 처음부터 끝까지 순서대로 전부 전송 — 슬라이딩 윈도우가
    // 아니라 고정 크기 배치를 한꺼번에 다 쏘는 방식 (fsm-design.md §6).
    // 중간에 개별 ACK를 "기다리지"는 않지만(그러면 배치 방식 자체가 무의미),
    // 아래처럼 짧은 간격으로 폴링은 한다 — 이유는 바로 아래 주석.
    // [버그 수정 2026-08-20] 예전에는 배치 안 모든 슬롯의 sentAtMs를 배치
    // 시작 시각(nowMs) 하나로 통일해서 찍었다. 그런데 실제로는 슬롯마다
    // chunkDelayMs(예: 40ms)씩 늦게 전송되므로, 배치 끝쪽 슬롯일수록
    // "실제 보낸 시각"과 "기록된 sentAtMs" 사이 오차가 커진다(마지막
    // 슬롯은 최대 (batchSize-1)*chunkDelayMs만큼 일찍 찍힘). 이 오차만큼
    // tickWaitingBatchAck()의 타임아웃 판정(nowMs - slot.sentAtMs >
    // m_timeoutMs)이 실제보다 일찍 만료된 것으로 오판해서, 응답이 아직
    // 오는 중인데도 불필요하게 재전송했다 — ESP32 담당자가 실기기
    // 테스트로 찾아냄. currentMs를 슬롯마다 실제로 쉰 만큼(step)
    // 전진시켜서 각 슬롯의 sentAtMs가 "그 슬롯을 진짜로 보낸 시각"을
    // 반영하게 한다. 시스템 시계(otaSessionNowMs())를 안 쓰고 nowMs
    // 기준 논리 시계를 계속 쓰는 이유는 pollDuringDelay() 주석 참고 —
    // 유닛테스트가 가짜 nowMs로 타임아웃을 검증하는 구조를 깨지 않기
    // 위함.
    int64_t currentMs = nowMs;
    for (auto &slot : m_batch) {
        // [2026-08-23 정정, 실기기 FHSS 통합 테스트에서 재현] 예전엔 여기서
        // 한 번만 send()를 부르고 실패하면 바로 fail()로 세션 전체를
        // 죽였다 — FHSS 호핑 중엔 hop_worker의 SYNC 송신과 겹치는 -EBUSY가
        // 흔해서(design-notes 42절 6차 시도, 실제로 seq=4에서 재현됨),
        // 이 경로가 사실상 항상 세션을 죽이는 셈이었다. sendWithRetry()로
        // 짧게 재시도한 뒤에도 실패해야만 진짜 fail()로 처리한다.
        if (!sendWithRetry(slot.packet, "seq=" + std::to_string(slot.sequence) + " 배치 전송")) {
            fail("OTA_DATA 전송 실패 (sequence=" + std::to_string(slot.sequence) + ")");
            return;
        }
        slot.sentAtMs = currentMs;

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
        if (!pollDuringDelay(&currentMs))
            return; // fail() 처리됨 — 배치 나머지 전송 중단
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

bool OtaSession::retransmitSlot(BatchSlot &slot, int64_t nowMs, const char *reason)
{
    ++slot.retryCount;
    if (slot.retryCount > m_maxRetry) {
        fail("배치 재전송 한도(" + std::to_string(m_maxRetry) + "회) 초과 (sequence=" +
             std::to_string(slot.sequence) + ")");
        return false;
    }
    log("seq=" + std::to_string(slot.sequence) + " 재전송 (사유=" + reason + ", 재시도 "
        + std::to_string(slot.retryCount) + "/" + std::to_string(m_maxRetry) + ")");
    // RETRANSMITTING은 이 슬롯 하나 재전송하는 동안만 순간적으로 거쳐감
    // (fsm-design.md 상태 다이어그램: RETRANSMITTING -> WAITING_BATCH_ACK).
    setState(OtaSessionState::Retransmitting);
    (void)sendWithRetry(slot.packet, "seq=" + std::to_string(slot.sequence) + " 재전송");
    slot.sentAtMs = nowMs;

    // [추가 2026-08-19] 재전송에도 배치 전송과 같은 간격을 둔다.
    // enterSendingBatch()는 청크 사이에 chunkDelayMs를 넣는데 여기만 빠져
    // 있었다 — 그래서 재전송이 쉬는 시간 없이 붙어 나갔고, 반이중인 CC1101이
    // 그동안 돌아온 ACK를 못 들었다(위 tickWaitingBatchAck() 주석 참고).
    //
    // [버그 수정 2026-08-20] 위 sleep_for()만 있고 recv() 폴링이 없었다 —
    // enterSendingBatch()는 2026-08-19에 이미 폴링을 넣었는데(위 주석) 이
    // 재전송 경로엔 그 수정이 빠져 있었다. ESP32 담당자가 실기기 테스트로
    // 찾아냄: 재전송한 DATA에 대한 ACK/NACK이 바로 도착해도 이 sleep
    // 구간 동안은 못 듣고 흘려보내서, 다음 tick의 타임아웃에서야(최악
    // m_timeoutMs만큼 늦게) 반영됐다. enterSendingBatch()와 같은
    // pollDuringDelay()를 재사용해서 여기도 5ms 간격으로 폴링하도록 맞춘다.
    int64_t currentMs = nowMs;
    if (!pollDuringDelay(&currentMs))
        return false; // fail() 처리됨 (폴링 중 다른 슬롯이 재전송 한도 초과)

    setState(OtaSessionState::WaitingBatchAck);
    return true;
}

bool OtaSession::pollAndApplyAckOrNack(int64_t nowMs, bool *hadPacket)
{
    const auto packet = tryReceiveOnce(m_transport);
    // 큐가 비어 있었는지(hadPacket=false) 뭔가 읽었는지(true)를 기록 —
    // tryReceiveOnce()는 아직 도착한 게 없으면 kind==Unknown && raw가
    // 빈 상태로 돌려준다(simplereceiver.h 주석). drainAckOrNackQueue()가
    // 이 값으로 "더 읽을 게 남았는지" 판단한다.
    if (hadPacket)
        *hadPacket = !(packet.kind == ReceivedPacketKind::Unknown && packet.raw.empty());
    // [버그 수정 2026-08-20, ESP32 담당자 리포트 클레임 1] acknowledged_type이
    // OTA_PKT_DATA인 응답만 배치 슬롯에 매칭한다. 예전엔 이 필드를 확인 안
    // 하고 sequence만 봤는데, sequence 하나만으로는 "이게 START/END에 대한
    // 응답인데 우연히 지금 배치의 어느 슬롯 sequence와 같은 값이 되는" 경우를
    // 걸러낼 수 없었다 — OTA_CONTROL_SEQUENCE가 작은 배치 sequence(0,1,2...)와
    // 겹칠 수치는 아니라 실제로 재현되진 않았지만, 프로토콜이 명시적으로
    // 실어 보내는 구분 정보를 검증 안 하고 버리는 건 잠재 버그였다.
    if ((packet.kind == ReceivedPacketKind::Ack || packet.kind == ReceivedPacketKind::Nack)
        && packet.sessionId == m_sessionId
        && packet.acknowledgedType == static_cast<uint8_t>(OTA_PKT_DATA)) {
        for (auto &slot : m_batch) {
            if (slot.acked || slot.sequence != packet.sequence)
                continue;

            const bool ok = (packet.kind == ReceivedPacketKind::Ack
                              && packet.resultCode == static_cast<uint8_t>(OTA_RESULT_OK));
            if (ok) {
                slot.acked = true;
                ++m_totalAcked;
                log("seq=" + std::to_string(slot.sequence) + " ACK 수신 ("
                    + std::to_string(m_totalAcked) + "/" + std::to_string(m_chunks.size()) + ")");
            } else {
                log("seq=" + std::to_string(slot.sequence) + " NACK 수신 (result_code="
                    + std::to_string(static_cast<int>(packet.resultCode)) + ")");
                // NACK 이중 방어 — 타임아웃을 기다리지 않고 그 슬롯만 즉시 재전송
                // (fsm-design.md §6 "슬롯별 독립 타이머 + NACK 이중 방어")
                if (!retransmitSlot(slot, nowMs, "NACK"))
                    return false; // fail() 처리됨
            }
            break;
        }
    }
    return true;
}

bool OtaSession::drainAckOrNackQueue(int64_t nowMs)
{
    // [2026-08-20 추가, 실기기 재현 버그 수정] 왜 필요한가: ESP32는 배치가
    // 아직 안 끝났으면 500ms 간격으로 "아직 못 받음" NACK을 반복 전송한다.
    // 그 사이 실제 데이터가 도착해서 성공 ACK이 뒤따라오면, 파이의 CC1101
    // 커널 드라이버 수신 버퍼(RX 큐)에는 낡은 NACK 여러 개와 최신 ACK이
    // 같이 쌓인다. 예전에는 tickWaitingBatchAck()이 한 틱에 딱 하나만
    // 읽었는데, 그러면 낡은 NACK들을 하나씩 처리하며 재시도 횟수(retryCount)
    // 를 다 써버리고, 정작 바로 뒤에 있는 최신 ACK은 다음 틱에서야(혹은
    // 재시도 한도 초과로 아예 못 보고) 처리됐다.
    //
    // 2026-08-20 실기기 로그(design-notes 37절, seq=959)로 실제 실패가
    // 확인됨: ESP32는 이미 ACK을 두 번 보냈는데, Gateway는 그 앞에 쌓여
    // 있던 낡은 NACK 2개를 처리하다 재시도 한도를 넘겨 실패로 끝났다.
    //
    // 큐가 빌 때까지(또는 fail() 나거나 한도에 닿을 때까지) 계속 드레인하면,
    // 같은 틱 안에서 "낡은 NACK 처리 -> 재전송" 다음에 바로 "최신 ACK 처리
    // -> acked=true"까지 이어져서, 최신 정보가 항상 마지막에 반영된다.
    constexpr int kMaxDrainPerTick = 32; // 오작동한 transport가 recv()에서
        // 절대 빈 값을 안 주는 경우에도 무한루프에 빠지지 않기 위한 안전판.
        // 배치 크기(기본 5)보다 충분히 크게 잡음 — 슬롯 수보다 훨씬 많은
        // 중복/낡은 응답이 한꺼번에 쌓이는 경우는 실제로 없었음.
    for (int i = 0; i < kMaxDrainPerTick; ++i) {
        bool hadPacket = false;
        if (!pollAndApplyAckOrNack(nowMs, &hadPacket))
            return false; // fail() 처리됨
        if (!hadPacket)
            break; // 큐가 비었음 — 더 읽을 게 없음
    }
    return true;
}

bool OtaSession::pollDuringDelay(int64_t *nowMs)
{
    if (m_chunkDelayMs <= 0)
        return true;

    // [2026-08-20 추가] enterSendingBatch()와 retransmitSlot() 둘 다 쓰던
    // "5ms 간격 쪼개기 + 폴링" 로직을 여기 하나로 합쳤다(예전엔
    // enterSendingBatch()에만 있고 retransmitSlot()엔 없어서 버그였다 —
    // 위 retransmitSlot() 주석 참고).
    //
    // *nowMs를 실제로 잠든 만큼(step)만 전진시키는 이유: 여기서
    // otaSessionNowMs()(진짜 시스템 시계)를 부르면 더 "정확"해 보이지만,
    // 유닛테스트는 tick()에 작은 가짜 정수(예: 1010)를 nowMs로 넘겨서
    // sleep 없이 타임아웃 로직을 검증하는 구조다. 여기서 진짜 시계를
    // 섞으면 tickWaitingBatchAck()의 `nowMs - slot.sentAtMs` 계산이
    // (가짜 nowMs) - (진짜 큰 시스템 시각)처럼 뒤섞여서 타임아웃 판정이
    // 깨진다. 그래서 "호출한 쪽이 준 시간 기준을 그대로 유지한 채, 실제
    // 잠든 시간(ms)만큼만 더한다" — 진짜 하드웨어에서는 이 값이 곧
    // 실제 흐른 시간과 같고, 테스트에서는 가짜 기준 위에서 논리적으로
    // 같은 양만큼만 전진하므로 두 쪽 다 옳게 동작한다.
    constexpr int kPollIntervalMs = 5;
    int remainingMs = m_chunkDelayMs;
    while (remainingMs > 0) {
        const int step = std::min(remainingMs, kPollIntervalMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        remainingMs -= step;
        *nowMs += step;
        if (!pollAndApplyAckOrNack(*nowMs))
            return false; // fail() 처리됨
    }
    return true;
}

void OtaSession::tickWaitingBatchAck(int64_t nowMs)
{
    // 1. 수신 확인 — 큐에 쌓인 ACK/NACK을 빌 때까지 전부 드레인한다.
    // [수정 2026-08-20] 예전엔 한 틱에 패킷 하나만 처리했는데, 그러면 낡은
    // NACK 여러 개가 큐에 쌓여 있을 때 최신 ACK을 늦게 보거나 아예 재시도
    // 한도 초과로 못 보는 문제가 있었다 — drainAckOrNackQueue() 주석 참고.
    if (!drainAckOrNackQueue(nowMs))
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
        if (!retransmitSlot(slot, nowMs, "timeout"))
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
    // [2026-08-23 정정] sendStartPacket()과 같은 이유로 sendWithRetry() 사용.
    (void)sendWithRetry(std::vector<uint8_t>(packet, packet + written), "OTA_END");
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
    // [2026-08-20, 위 tickHandshaking() matchesOurStart와 같은 이유] END 응답도
    // acknowledged_type을 같이 확인해서 START 응답과 혼동될 여지를 없앤다.
    const bool matchesOurEnd =
        packet.sessionId == m_sessionId && packet.sequence == OTA_CONTROL_SEQUENCE
        && packet.acknowledgedType == static_cast<uint8_t>(OTA_PKT_END);

    if (packet.kind == ReceivedPacketKind::Ack && matchesOurEnd) {
        log("END ACK 수신 -> Completed (session_id=0x" + toHex(m_sessionId) + ")");
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
        log("END 응답 타임아웃 (재시도 " + std::to_string(m_controlRetryCount + 1) + "/"
            + std::to_string(m_maxRetry) + ")");
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
