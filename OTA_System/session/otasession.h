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

    // [2026-08-20 추가] ACK/NACK 수신·재전송·타임아웃처럼 상태 전환보다
    // 더 촘촘한 이벤트를 한 줄 문자열로 받는 콜백(선택). ESP32 실기기
    // 테스트에서 "seq3 ACK 처리 실패"처럼 어디서 막히는지 겉으로 안 보여서
    // (StateCallback만으로는 WaitingBatchAck 안에서 무슨 일이 있었는지
    // 전혀 알 수 없음) 진단용으로 추가함 — CLI 스모크테스트는 stdout에,
    // otamanager.cpp는 기존 로그 카드에 그대로 이어붙이면 됨.
    using LogCallback = std::function<void(const std::string &)>;
    void setOnLog(LogCallback cb) { m_onLog = std::move(cb); }

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
    LogCallback m_onLog;
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
    // m_onLog가 설정돼 있으면 그대로 전달, 아니면 조용히 무시.
    void log(const std::string &msg) const;

    // [2026-08-23 추가, 실기기 FHSS 통합 테스트에서 확인] FHSS 호핑 중에는
    // 커널 hop_worker가 매 슬롯 스스로 SYNC를 보내는데, 그 순간과 겹치면
    // transport.send()가 일시적으로 실패한다(-EBUSY, kernel-cc1101-spi
    // cc1101_write() 315~318행). 진짜 오류가 아니라 "그 찰나만 바쁘다"는
    // 것뿐이라 짧게 쉬었다 재시도하면 대부분 통과한다 — sendStartPacket()/
    // sendEndPacket()/retransmitSlot()/배치 전송이 전부 이걸 거쳐가도록
    // 통일한다(design-notes-gateway-ota-es.md 42절 6차 시도 참고).
    // context: 로그에 남길 설명 문자열("OTA_START" 등).
    bool sendWithRetry(const std::vector<uint8_t> &packet, const std::string &context);

    void enterHandshaking(int64_t nowMs);
    void tickHandshaking(int64_t nowMs);
    void sendStartPacket();

    void enterSendingBatch(int64_t nowMs); // 배치를 채우고 즉시 전부 전송 -> WaitingBatchAck
    void tickWaitingBatchAck(int64_t nowMs);
    // 슬롯 하나를 재전송. retryCount가 maxRetry를 넘으면 fail() 처리하고 false 반환.
    // reason: 로그에만 쓰는 문자열("NACK"/"timeout") — 왜 재전송이 트리거됐는지
    // 구분하기 위함(2026-08-20 로그 추가).
    bool retransmitSlot(BatchSlot &slot, int64_t nowMs, const char *reason);
    // ACK/NACK 패킷 하나를 논블로킹으로 폴링해서, 있으면 현재 배치(m_batch)에
    // 반영한다(tickWaitingBatchAck()의 "1. 수신 확인" 단계와
    // enterSendingBatch()의 배치 전송 중 폴링이 이 로직을 공유하기 위해 분리함
    // — 2026-08-19 전송 효율 개선, 아래 enterSendingBatch() 주석 참고).
    // 재전송 한도 초과로 fail()이 호출됐으면 false를 반환 — 호출부는 이후
    // 처리를 즉시 중단해야 한다.
    // hadPacket(선택, 2026-08-20 추가): 널이 아니면, 이번 호출에서 실제로
    // 패킷을 하나 읽었는지(true) 아니면 큐가 비어 있었는지(false)를 채워
    // 준다 — drainAckOrNackQueue()가 "더 읽을 게 남았는지" 판단하는 데 씀.
    bool pollAndApplyAckOrNack(int64_t nowMs, bool *hadPacket = nullptr);

    // [2026-08-20 추가, 실기기 재현 버그 수정] tickWaitingBatchAck()가 한
    // 틱에 응답 패킷을 딱 하나만 처리하던 걸, 큐가 빌 때까지(또는 fail()
    // 날 때까지) 전부 드레인하도록 바꿈. 이유: ESP32는 배치가 아직 안
    // 끝났으면 500ms 간격으로 "아직 못 받음" NACK을 반복 전송하는데,
    // 그 사이 실제 데이터가 도착해 ACK가 뒤따라오면 파이의 CC1101 커널
    // 드라이버 수신 버퍼에 낡은 NACK 여러 개 + 최신 ACK이 같이 쌓인다.
    // 한 틱에 하나씩만 처리하면 낡은 NACK들을 처리하는 동안 재시도
    // 횟수를 다 써버려서, 바로 뒤에 있는 최신 ACK을 보기도 전에
    // "재전송 한도 초과"로 실패할 수 있다 — 2026-08-20 실기기 로그
    // (seq=959)로 확인됨: ESP32는 이미 ACK을 두 번 보냈는데 Gateway는
    // 그 앞에 쌓여 있던 낡은 NACK 2개를 처리하다 실패로 끝났다.
    // 최대 kMaxDrainPerTick번까지만 반복해서, 잘못된 transport 구현이
    // recv()에서 절대 빈 값을 안 주는 경우에도 무한루프에 빠지지 않게 함.
    // 재전송 한도 초과로 fail()이 호출됐으면 false를 반환한다.
    bool drainAckOrNackQueue(int64_t nowMs);

    // chunkDelayMs만큼 5ms 간격으로 쪼개 폴링하며 대기한다. *nowMs를 실제로
    // 잠든 만큼(step)만 전진시켜서, 대기 중 poll로 다른 슬롯이 재전송되면
    // 그 슬롯의 sentAtMs도 "그 시점의 진짜 지금"을 반영하게 한다 — 진짜
    // 시스템 시계(otaSessionNowMs())를 쓰지 않는 이유는, 유닛테스트가
    // tick()에 넘기는 가짜(논리) nowMs 값으로 타임아웃을 검증하는 구조라서
    // 여기서 실제 시계를 섞으면 그 타임아웃 계산이 깨지기 때문이다.
    // enterSendingBatch()(최초 전송 사이 대기)와 retransmitSlot()(재전송
    // 직후 대기, 2026-08-20 추가)이 이 로직을 공유한다.
    // 재전송 한도 초과로 fail()이 호출됐으면 false를 반환한다.
    bool pollDuringDelay(int64_t *nowMs);

    void enterWaitingEndAck(int64_t nowMs);
    void tickWaitingEndAck(int64_t nowMs);
    void sendEndPacket();
};

// std::chrono::steady_clock 기반 현재 시각(ms). 실제 앱(otamanager.cpp)에서
// tick()/pause()/resume()에 넘길 값을 구할 때 씀 — 테스트는 이 헬퍼 없이 직접
// 가짜 값을 넣습니다.
int64_t otaSessionNowMs();

#endif // OTASESSION_H
