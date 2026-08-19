#ifndef OTASESSION_H
#define OTASESSION_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "binsplitter.h" // OtaChunk

class ITransport;

// docs/fsm-design.md의 송신측 FSM("OtaSession") 구현체.
//
// [의도적으로 Qt 의존성 없음] core/·session/의 다른 파일과 같은 이유 —
// g++/clang++만으로 컴파일·테스트 가능해야 tests/tst_otasession.cpp가 실기기
// 없이도 로직만 검증할 수 있음.
//
// simplesender.h의 simpleSendFile()/performHandshake()는 "이 뼈대가 실기기에서
// 실제로 동작하는가"를 확인한 스모크테스트였고(2026-08-16/17 검증 완료,
// docs/roadmap.md 4절), 이 클래스는 그 위에 신뢰성 있는 전송(배치 ACK +
// 누락분만 선택적 재전송)을 얹습니다. START/END 인코딩처럼 겹치는 부분은 새로
// 안 만들고 simplesender.cpp의 값 구조를 그대로 따릅니다.
//
// 이 클래스가 구현하는 상태는 docs/fsm-design.md 표 전체가 아니라 "세션이 이미
// 시작된 이후" 부분만입니다. DISCONNECTED/CONNECTED_IDLE/FILE_READY/
// DISCOVERING/SELECTING은 연결·파일선택·기기조회처럼 화면(otamanager.cpp)이
// 담당할 상위 흐름이라 여기 안 들어갑니다(docs/roadmap.md 5절, "다음에 할 일"
// 9번 DISCOVER 항목 — 아직 미착수). target_device_id는 호출부가 이미 알고
// 있다고 가정하고 start()에서 바로 받습니다.
enum class OtaSessionState {
    Idle,              // start() 호출 전
    Handshaking,       // fsm-design.md: HANDSHAKING
    SendingBatch,      // SENDING_BATCH — 배치를 채워서 전부 보내는 동안만 잠깐 거쳐감
    WaitingBatchAck,   // WAITING_BATCH_ACK
    Retransmitting,    // RETRANSMITTING — WaitingBatchAck 루프 안에서 슬롯 하나 재전송할 때만
                        // 순간적으로 거쳐가는 상태 (fsm-design.md §6 "재해석" 참고 —
                        // "배치 확인 패킷 하나"가 아니라 슬롯별 개별 ACK/NACK/타임아웃 처리 결과)
    WaitingEndAck,     // WAITING_END_ACK
    Paused,
    Completed,
    Failed,
};

// 로그/디버깅용 상태 이름. otamanager.cpp의 로그 카드에서 그대로 찍으면 됨.
const char *otaSessionStateName(OtaSessionState state);

struct OtaSessionProgress
{
    uint32_t ackedChunks = 0;       // 지금까지 ACK 확정된 청크 수 (배치 경계 무관, 누적)
    uint32_t totalChunks = 0;
    uint32_t currentBatchNumber = 0; // 1-based. 0이면 아직 시작 안 함
    uint32_t totalBatches = 0;
};

class OtaSession
{
public:
    // batchSize/timeoutMs/maxRetry 기본값은 docs/fsm-design.md 결정 이력
    // (2026-08-11)의 batchSize=5, "300ms x 5회, 모든 단계 통일"을 그대로 씀.
    //
    // chunkDelayMs: fsm-design.md 작성 시점(2026-08-11)에는 없던 값입니다.
    // 그 뒤 실기기 검증(2026-08-17, docs/roadmap.md 4절)에서 배치 안 청크를
    // 쉬지 않고 연속으로 쏘면(0ms) 수신측 처리가 못 따라가 패킷 경계가
    // 무너지는 게 확인됐습니다 — 그래서 배치 내부 전송에도 같은 간격을 둡니다.
    // session/simplesender.h의 chunkDelayMs와 같은 이유·같은 기본값(40ms).
    explicit OtaSession(
        ITransport &transport,
        int batchSize = 5,
        int timeoutMs = 300,
        int maxRetry = 5,
        int chunkDelayMs = 40);

    // FILE_READY -> HANDSHAKING 진입에 해당. 내부에서 파일 크기 확인 +
    // BinSplitter::split()까지 수행합니다 (chunkSize는 항상 OTA_MAX_PAYLOAD_SIZE —
    // simplesender.h와 달리 이 클래스는 아직 chunkSize를 파라미터로 안 받습니다,
    // 필요해지면 나중에 추가).
    //
    // 실패하면 false를 반환하고 상태는 Idle 그대로 유지합니다(다시 호출 가능).
    // nowMs: 기본값은 실제 시각(otaSessionNowMs()) — 테스트에서만 직접 값을 넣음.
    bool start(const std::string &filePath, uint32_t targetDeviceId, uint32_t sessionId = 0,
               int64_t nowMs = -1);

    // 매 틱마다 호출 (Qt라면 QTimer로 주기 호출, 예: 10ms 간격). 논블로킹 —
    // transport.recv()를 한 번만 폴링하고 바로 리턴합니다.
    //
    // nowMs: 호출부가 넘기는 현재 시각(ms). 실제 앱에서는 otaSessionNowMs()를
    // 쓰면 되고, 테스트에서는 가짜 시각을 넣어서 타임아웃을 실제로 기다리지
    // 않고도 재전송/재시도-초과 로직을 검증할 수 있습니다.
    void tick(int64_t nowMs);

    // SendingBatch/WaitingBatchAck/Retransmitting/WaitingEndAck 중일 때만 유효
    // (그 외 상태에서는 조용히 무시).
    void pause(int64_t nowMs);
    // 멈춰 있던 동안 흐른 시간만큼 타이머를 지금 시각 기준으로 다시 맞춥니다 —
    // 안 하면 재개하자마자 오래 멈춰 있던 슬롯들이 전부 타임아웃으로 잡혀서
    // 불필요한 재전송이 한꺼번에 일어납니다.
    void resume(int64_t nowMs);

    OtaSessionState state() const { return m_state; }
    OtaSessionProgress progress() const;
    const std::string &errorMessage() const { return m_errorMessage; }
    uint32_t sessionId() const { return m_sessionId; }

    // 상태가 바뀔 때마다 호출되는 콜백(선택). otamanager.cpp가 이걸 구독해서
    // 진행률 바·로그·버튼 상태만 갱신하면 됩니다(fsm-design.md §1 "화면과
    // 로직의 경계" 원칙 그대로).
    using StateCallback = std::function<void(OtaSessionState)>;
    void setOnStateChanged(StateCallback cb) { m_onStateChanged = std::move(cb); }

private:
    struct BatchSlot
    {
        uint32_t sequence = 0;
        std::vector<uint8_t> packet; // 재전송용으로 그대로 들고 있음
        int64_t sentAtMs = 0;        // 마지막으로 보낸 시각 (재전송마다 갱신)
        int retryCount = 0;
        bool acked = false;
    };

    ITransport &m_transport;
    const int m_batchSize;
    const int m_timeoutMs;
    const int m_maxRetry;
    const int m_chunkDelayMs;

    OtaSessionState m_state = OtaSessionState::Idle;
    OtaSessionState m_pausedFrom = OtaSessionState::Idle;
    StateCallback m_onStateChanged;
    std::string m_errorMessage;

    uint32_t m_targetDeviceId = 0;
    uint32_t m_sessionId = 0;
    uint32_t m_imageSize = 0;
    uint8_t m_imageSha256[32] = {};   // start()에서 sha256File()로 채움 (core/sha256.h)
    std::vector<OtaChunk> m_chunks;   // BinSplitter 결과, 전체 청크
    uint32_t m_nextUnsentIndex = 0;   // m_chunks 기준 다음 배치가 시작할 인덱스

    std::vector<BatchSlot> m_batch;   // 현재 배치

    int64_t m_controlSentAtMs = 0;    // HANDSHAKING/WAITING_END_ACK 공용 (START/END 재전송 타이머)
    int m_controlRetryCount = 0;

    uint32_t m_totalAcked = 0;        // progress() 표시용 누적 카운터
    uint32_t m_currentBatchNumber = 0; // progress() 표시용 (1-based)

    void setState(OtaSessionState s);
    void fail(const std::string &reason);

    void enterHandshaking(int64_t nowMs);
    void tickHandshaking(int64_t nowMs);
    void sendStartPacket();

    void enterSendingBatch(int64_t nowMs); // 배치를 채우고 즉시 전부 전송 -> WaitingBatchAck
    void tickWaitingBatchAck(int64_t nowMs);
    // 슬롯 하나를 재전송. retryCount가 maxRetry를 넘으면 fail() 처리하고 false 반환.
    bool retransmitSlot(BatchSlot &slot, int64_t nowMs);

    void enterWaitingEndAck(int64_t nowMs);
    void tickWaitingEndAck(int64_t nowMs);
    void sendEndPacket();
};

// std::chrono::steady_clock 기반 현재 시각(ms). 실제 앱(otamanager.cpp)에서
// tick()/pause()/resume()에 넘길 값을 구할 때 씀 — 테스트는 이 헬퍼 없이 직접
// 가짜 값을 넣습니다.
int64_t otaSessionNowMs();

#endif // OTASESSION_H
