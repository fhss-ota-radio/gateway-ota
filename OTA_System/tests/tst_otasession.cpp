/*
 * OtaSession(docs/fsm-design.md 송신측 FSM)을 검증하는 테스트.
 * tst_binsplitter.cpp와 같은 assert 기반 스타일, Qt 의존성 없음.
 *
 * 실기기(CC1101) 없이 로직만 검증하기 위해 FakeTransport(아래)를 씁니다 —
 * transport.recv()가 테스트가 미리 큐에 넣어둔 ACK/NACK 바이트를 그대로
 * 돌려주는 인메모리 스크립트 방식입니다. 타임아웃/재시도도 실제로 기다리지
 * 않습니다 — tick(nowMs)가 시각을 파라미터로 받으므로, 테스트는 가짜 시각을
 * 직접 증가시켜서 "300ms 지남"을 즉시 흉내냅니다.
 *
 *   g++ -std=c++17 -Wall -Wextra \
 *       -I.. -I../core -I../transport -I../session -I../../../ota-protocol/include \
 *       tst_otasession.cpp ../session/otasession.cpp ../session/simplesender.cpp \
 *       ../session/simplereceiver.cpp ../core/binsplitter.cpp -o tst_otasession
 *   ./tst_otasession
 *
 * CMake로 빌드하면(ota_session_tests 타깃) 위 include 경로는 CMakeLists.txt가 대신 잡아줍니다.
 */
#include "otasession.h"

#include "itransport.h"

#include <cassert>
#include <cstdio>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include "ota_protocol.h"
}

namespace {

// ---------- 테스트 유틸 ----------

std::string writeTempFile(const std::string &name, const std::vector<uint8_t> &content)
{
    const std::string path = "/tmp/" + name;
    FILE *f = std::fopen(path.c_str(), "wb");
    assert(f != nullptr);
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return path;
}

std::vector<uint8_t> repeat(uint8_t value, size_t count)
{
    return std::vector<uint8_t>(count, value);
}

std::vector<uint8_t> makeAckOrNack(ota_packet_type_t type, uint32_t sessionId,
                                    uint8_t ackedType, uint32_t sequence,
                                    uint8_t resultCode = OTA_RESULT_OK)
{
    ota_ack_fields_t fields{};
    fields.session_id = sessionId;
    fields.acknowledged_type = ackedType;
    fields.sequence = sequence;
    fields.result_code = resultCode;

    uint8_t buf[OTA_ACK_PACKET_SIZE];
    const size_t written = ota_protocol_encode_ack(buf, sizeof(buf), type, &fields);
    assert(written == OTA_ACK_PACKET_SIZE);
    return std::vector<uint8_t>(buf, buf + written);
}

// 실기기(CC1101) 대신 쓰는 인메모리 ITransport. send()는 그냥 기록만 하고,
// recv()는 rxQueue에 미리 넣어둔 걸 하나씩 꺼내 돌려줌(비어있으면 빈 벡터 —
// tryReceiveOnce()가 "아직 도착한 거 없음"으로 해석하는 것과 동일한 규약).
class FakeTransport : public ITransport
{
public:
    bool open() override { return true; }
    void close() override {}
    bool isOpen() const override { return true; }

    bool send(const std::vector<uint8_t> &data) override
    {
        sentPackets.push_back(data);
        return true;
    }

    std::vector<uint8_t> recv() override
    {
        if (rxQueue.empty())
            return {};
        auto front = rxQueue.front();
        rxQueue.pop_front();
        return front;
    }

    // 이 세션이 보낸 패킷 중 type byte가 t인 것의 개수 (예: OTA_PKT_DATA가
    // 몇 번 보내졌는지로 재전송 횟수를 셈).
    int countSentOfType(ota_packet_type_t t) const
    {
        int n = 0;
        for (const auto &p : sentPackets)
            if (!p.empty() && p[0] == static_cast<uint8_t>(t))
                ++n;
        return n;
    }

    std::deque<std::vector<uint8_t>> rxQueue;
    std::vector<std::vector<uint8_t>> sentPackets;
};

// ---------- 테스트 ----------

// 3청크(48,48,34byte)를 batchSize=2로 보내서, 배치 경계를 두 번 넘기고
// 핸드셰이크 -> 배치 2번 -> END까지 전체 흐름이 끝까지 도는지 확인.
void happyPathCompletesAcrossMultipleBatches()
{
    const std::string path = writeTempFile("os_happy.bin", repeat('A', 48 * 2 + 34)); // 3청크
    constexpr uint32_t kSessionId = 0x11111111u;

    FakeTransport transport;
    OtaSession session(transport, /*batchSize=*/2, /*timeoutMs=*/300, /*maxRetry=*/5,
                        /*chunkDelayMs=*/0);

    assert(session.start(path, /*targetDeviceId=*/1, kSessionId, /*nowMs=*/1000));
    assert(session.state() == OtaSessionState::Handshaking);
    assert(transport.countSentOfType(OTA_PKT_START) == 1);

    // START ACK -> 첫 배치(seq0,1) 전부 전송되고 WaitingBatchAck로
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010);
    assert(session.state() == OtaSessionState::WaitingBatchAck);
    assert(transport.countSentOfType(OTA_PKT_DATA) == 2);

    // seq0, seq1 ACK -> 두 번째 배치(seq2) 전송
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));
    session.tick(1020);
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 1));
    session.tick(1030);
    assert(session.state() == OtaSessionState::WaitingBatchAck);
    assert(transport.countSentOfType(OTA_PKT_DATA) == 3);
    assert(session.progress().ackedChunks == 2);

    // seq2 ACK -> 전 청크 완료, END 전송
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 2));
    session.tick(1040);
    assert(session.state() == OtaSessionState::WaitingEndAck);
    assert(transport.countSentOfType(OTA_PKT_END) == 1);

    // END ACK -> Completed
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_END, OTA_CONTROL_SEQUENCE));
    session.tick(1050);
    assert(session.state() == OtaSessionState::Completed);
    assert(session.progress().ackedChunks == 3);
    assert(session.progress().totalChunks == 3);

    std::remove(path.c_str());
    std::cout << "[OK] happyPathCompletesAcrossMultipleBatches\n";
}

// 청크 하나만 NACK -> 그 슬롯만 즉시 재전송(타임아웃 안 기다림). 같은 배치의
// 다른 슬롯은 건드리지 않는지 확인 (Selective-Repeat, Go-Back-N 아님).
void nackRetransmitsOnlyThatChunkImmediately()
{
    const std::string path = writeTempFile("os_nack.bin", repeat('B', 48 + 10)); // 2청크
    constexpr uint32_t kSessionId = 0x22222222u;

    FakeTransport transport;
    OtaSession session(transport, /*batchSize=*/2, 300, 5, 0);

    session.start(path, 1, kSessionId, 1000);
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010);
    assert(transport.countSentOfType(OTA_PKT_DATA) == 2); // seq0, seq1 최초 전송

    // seq0만 NACK
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_NACK, kSessionId, OTA_PKT_DATA, 0, OTA_RESULT_INVALID_CRC));
    session.tick(1020);
    assert(session.state() == OtaSessionState::WaitingBatchAck); // Retransmitting을 거쳐 바로 복귀
    assert(transport.countSentOfType(OTA_PKT_DATA) == 3); // seq0 재전송 1회 추가 (seq1은 그대로)

    // 이제 둘 다 ACK
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));
    session.tick(1030);
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 1));
    session.tick(1040);
    assert(session.state() == OtaSessionState::WaitingEndAck);

    std::remove(path.c_str());
    std::cout << "[OK] nackRetransmitsOnlyThatChunkImmediately\n";
}

// ACK/NACK이 아예 안 와도(무선 유실 흉내) 타임아웃이 지나면 같은 슬롯을
// 재전송하는지 확인 — nowMs를 가짜로 진행시켜서 실제로 기다리지 않고 검증.
void timeoutTriggersResendOfSameChunk()
{
    const std::string path = writeTempFile("os_timeout.bin", repeat('C', 10)); // 1청크
    constexpr uint32_t kSessionId = 0x33333333u;

    FakeTransport transport;
    OtaSession session(transport, /*batchSize=*/1, /*timeoutMs=*/100, /*maxRetry=*/2, 0);

    session.start(path, 1, kSessionId, 1000);
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010); // DATA(seq0) 최초 전송, sentAtMs=1010
    assert(transport.countSentOfType(OTA_PKT_DATA) == 1);

    session.tick(1060); // 아직 100ms 안 지남 -> 재전송 없음
    assert(transport.countSentOfType(OTA_PKT_DATA) == 1);

    session.tick(1120); // 1120-1010=110ms > 100ms -> 재전송
    assert(transport.countSentOfType(OTA_PKT_DATA) == 2);
    assert(session.state() == OtaSessionState::WaitingBatchAck);

    // 이제 응답 -> 완료 진행
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));
    session.tick(1130);
    assert(session.state() == OtaSessionState::WaitingEndAck);

    std::remove(path.c_str());
    std::cout << "[OK] timeoutTriggersResendOfSameChunk\n";
}

// 재전송이 maxRetry를 넘으면 Failed로 가는지 (배치 단계).
void batchMaxRetryExceededLeadsToFailed()
{
    const std::string path = writeTempFile("os_maxretry.bin", repeat('D', 10)); // 1청크
    constexpr uint32_t kSessionId = 0x44444444u;

    FakeTransport transport;
    OtaSession session(transport, /*batchSize=*/1, /*timeoutMs=*/50, /*maxRetry=*/1, 0);

    session.start(path, 1, kSessionId, 1000);
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010); // DATA 최초 전송(sentAtMs=1010)

    session.tick(1070); // 60ms 지남 -> 재시도 1회째 (retryCount=1, maxRetry=1이라 허용)
    assert(session.state() == OtaSessionState::WaitingBatchAck);
    assert(session.errorMessage().empty());

    session.tick(1140); // 다시 60ms+ 지남 -> 재시도 2회째, maxRetry(1) 초과 -> Failed
    assert(session.state() == OtaSessionState::Failed);
    assert(!session.errorMessage().empty());

    std::remove(path.c_str());
    std::cout << "[OK] batchMaxRetryExceededLeadsToFailed\n";
}

// 핸드셰이크(START) 단계에서도 같은 재시도-초과 -> Failed 규칙이 적용되는지.
void handshakeMaxRetryExceededLeadsToFailed()
{
    const std::string path = writeTempFile("os_hstimeout.bin", repeat('E', 10));
    constexpr uint32_t kSessionId = 0x55555555u;

    FakeTransport transport;
    OtaSession session(transport, 1, /*timeoutMs=*/50, /*maxRetry=*/1, 0);

    session.start(path, 1, kSessionId, 1000); // START 최초 전송(sentAtMs=1000), ACK 응답 없음(무응답 흉내)

    session.tick(1060); // 재시도 1회
    assert(session.state() == OtaSessionState::Handshaking);
    assert(transport.countSentOfType(OTA_PKT_START) == 2);

    session.tick(1130); // 재시도 2회째 -> maxRetry(1) 초과 -> Failed
    assert(session.state() == OtaSessionState::Failed);
    assert(session.errorMessage().find("핸드셰이크") != std::string::npos);

    std::remove(path.c_str());
    std::cout << "[OK] handshakeMaxRetryExceededLeadsToFailed\n";
}

// OTA_END에 대한 NACK은 재시도 없이 바로 Failed로 가야 함 (SHA256 불일치 같은
// 재전송으로 못 고치는 오류 취급 — fsm-design.md 근거).
void endNackGoesDirectlyToFailedWithoutRetry()
{
    const std::string path = writeTempFile("os_endnack.bin", repeat('F', 10)); // 1청크
    constexpr uint32_t kSessionId = 0x66666666u;

    FakeTransport transport;
    OtaSession session(transport, 1, 300, 5, 0);

    session.start(path, 1, kSessionId, 1000);
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010);
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));
    session.tick(1020);
    assert(session.state() == OtaSessionState::WaitingEndAck);
    assert(transport.countSentOfType(OTA_PKT_END) == 1);

    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_NACK, kSessionId, OTA_PKT_END,
                                               OTA_CONTROL_SEQUENCE, OTA_RESULT_VERIFY_FAILED));
    session.tick(1030);
    assert(session.state() == OtaSessionState::Failed);
    assert(transport.countSentOfType(OTA_PKT_END) == 1); // 재전송 안 함

    std::remove(path.c_str());
    std::cout << "[OK] endNackGoesDirectlyToFailedWithoutRetry\n";
}

// pause() 이후 오래 지나서 resume()해도, 재개 시점 기준으로 타이머가 다시
// 맞춰져서 곧바로 스퓨리어스 타임아웃(불필요한 재전송)이 나지 않는지 확인.
void resumeDoesNotCauseSpuriousTimeout()
{
    const std::string path = writeTempFile("os_pause.bin", repeat('G', 10)); // 1청크
    constexpr uint32_t kSessionId = 0x77777777u;

    FakeTransport transport;
    OtaSession session(transport, 1, /*timeoutMs=*/100, 5, 0);

    session.start(path, 1, kSessionId, 1000);
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010); // DATA 최초 전송
    assert(transport.countSentOfType(OTA_PKT_DATA) == 1);

    session.pause(1020);
    assert(session.state() == OtaSessionState::Paused);

    // 타임아웃(100ms)보다 훨씬 오래(1초) 멈춰 있다가 재개
    session.resume(2020);
    assert(session.state() == OtaSessionState::WaitingBatchAck);

    session.tick(2030); // 재개 후 10ms밖에 안 지남 -> 재전송 없어야 함
    assert(transport.countSentOfType(OTA_PKT_DATA) == 1);

    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));
    session.tick(2040);
    assert(session.state() == OtaSessionState::WaitingEndAck);

    std::remove(path.c_str());
    std::cout << "[OK] resumeDoesNotCauseSpuriousTimeout\n";
}

// [효율 개선 2026-08-19] 배치 전송(enterSendingBatch) 도중에도 이미 도착한
// ACK를 그 자리에서 폴링해서 반영하는지 확인. 예전에는 배치를 다 보낼
// 때까지(chunkDelayMs만큼 sleep만 하고) recv()를 아예 안 불러서, 전송 중
// 도착한 ACK를 다음 WaitingBatchAck 틱까지 놓쳤다 — 반이중 CC1101 실기기
// 재검증(docs/note/design-notes-gateway-ota-es.md 27절)에서 중복 재전송
// (1067개 중 1062~1063개)의 주 원인으로 확인된 패턴.
//
// chunkDelayMs>0으로 설정해 폴링 구간이 실제로 생기게 하고, START ACK와
// slot0(seq0)의 DATA ACK를 미리 큐에 넣어둔 뒤, 이 배치를 촉발한 단 한
// 번의 tick() 호출 안에서 slot0이 이미 acked로 반영되는지 확인한다 —
// 옛 코드였다면 이 시점엔 아직 0이었을 것(다음 tick까지 큐에 남아있었을 것).
void batchSendingPollsAlreadyArrivedAckDuringSend()
{
    const std::string path = writeTempFile("os_pollduring.bin", repeat('H', 48 + 10)); // 2청크
    constexpr uint32_t kSessionId = 0x88888888u;

    FakeTransport transport;
    // chunkDelayMs=10ms (내부 폴링 간격 5ms 기준 슬롯당 2번 폴링) — 테스트가
    // 실제로 짧게(총 약 20ms) 기다리지만 무시할 수준.
    OtaSession session(transport, /*batchSize=*/2, /*timeoutMs=*/300, /*maxRetry=*/5,
                        /*chunkDelayMs=*/10);

    session.start(path, 1, kSessionId, 1000);

    // START ACK와 slot0 DATA ACK를 미리 큐에 순서대로 넣어둠 — slot0을
    // 보내고 나서 첫 폴링 구간에서 바로 소비될 것으로 기대.
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));

    session.tick(1010); // Handshaking -> SendingBatch(slot0,1 전송) -> WaitingBatchAck

    assert(session.state() == OtaSessionState::WaitingBatchAck);
    assert(transport.countSentOfType(OTA_PKT_DATA) == 2); // seq0,1 최초 전송만 (재전송 없음)
    assert(session.progress().ackedChunks == 1); // slot0이 배치 전송 "도중"에 이미 반영됨

    std::remove(path.c_str());
    std::cout << "[OK] batchSendingPollsAlreadyArrivedAckDuringSend\n";
}

// [버그 수정 2026-08-20, ESP32 담당자 실기기 테스트로 발견] retransmitSlot()이
// 재전송 직후 sleep_for(chunkDelayMs)만 하고 enterSendingBatch()와 달리
// recv()를 폴링하지 않던 버그 재현. NACK으로 재전송을 유발하면서, 그
// 재전송분에 대한 ACK을 미리 큐에 넣어 둔다 — 옛 코드였다면 이 ACK은
// retransmitSlot()의 sleep 구간엔 못 읽혀서 이 tick()이 끝난 시점엔 아직
// ackedChunks==0이었을 것(다음 tick의 최상단 폴링에서야 반영). 수정 후엔
// 재전송 직후 폴링 구간 안에서 같은 tick() 호출 중에 바로 소비된다.
void retransmitSlotPollsForResponseDuringItsOwnDelay()
{
    const std::string path = writeTempFile("os_retxpoll.bin", repeat('J', 48 + 10)); // 2청크
    constexpr uint32_t kSessionId = 0xAAAAAAAAu;

    FakeTransport transport;
    // chunkDelayMs=10 (내부 폴링 간격 5ms 기준 재전송 후 2번 폴링 기회) —
    // 2청크/batchSize=2로 해서 seq1이 남아있는 채로 WaitingBatchAck에
    // 머무는지까지 같이 확인한다.
    OtaSession session(transport, /*batchSize=*/2, /*timeoutMs=*/300, /*maxRetry=*/5,
                        /*chunkDelayMs=*/10);

    session.start(path, 1, kSessionId, 1000);
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010); // seq0,seq1 최초 전송 -> WaitingBatchAck
    assert(transport.countSentOfType(OTA_PKT_DATA) == 2);

    // seq0 NACK과, 재전송분에 대한 ACK을 순서대로 큐에 넣음 — 같은 tick()
    // 안에서 NACK 처리(재전송) -> 재전송 직후 폴링 구간에서 이 ACK까지
    // 바로 소비되는 게 기대 동작.
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_NACK, kSessionId, OTA_PKT_DATA, 0, OTA_RESULT_INVALID_CRC));
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));

    session.tick(1020);
    assert(transport.countSentOfType(OTA_PKT_DATA) == 3); // seq0 재전송 1회 추가
    assert(session.progress().ackedChunks == 1); // 재전송 직후 폴링에서 바로 반영돼야 함
    assert(session.state() == OtaSessionState::WaitingBatchAck); // seq1은 아직 응답 대기 중

    // 나머지도 정상 진행되는지(잔여 로직 안 깨졌는지) 확인
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 1));
    session.tick(1030);
    assert(session.state() == OtaSessionState::WaitingEndAck);

    std::remove(path.c_str());
    std::cout << "[OK] retransmitSlotPollsForResponseDuringItsOwnDelay\n";
}

// [버그 수정 2026-08-20, ESP32 담당자 실기기 테스트로 발견] enterSendingBatch()가
// 배치 안 모든 슬롯의 sentAtMs를 배치 "시작" 시각(nowMs 파라미터) 하나로
// 통일해서 찍던 버그 재현. slot0은 곧바로 ACK되게 하고, slot1은 응답 없이
// 놔둔 채 "slot1이 실제로 보내진 시각(진입 시각 + chunkDelayMs) 기준으로는
// 아직 타임아웃 전이지만, 옛 버그(배치 진입 시각 기준)로 계산하면 이미
// 타임아웃"인 nowMs를 골라서 tick()했을 때 불필요한 재전송이 안 나가는지
// 확인한다. 옛 코드였다면 이 tick에서 DATA가 한 번 더(3번째) 나갔을 것.
void batchSlotsGetIndividualSentAtMsNotSharedBatchStart()
{
    const std::string path = writeTempFile("os_slotstamp.bin", repeat('K', 48 + 10)); // 2청크
    constexpr uint32_t kSessionId = 0xBBBBBBBBu;

    FakeTransport transport;
    // chunkDelayMs=40 -> slot1은 배치 진입보다 실제로 ~40ms 늦게 나감.
    // timeoutMs=50으로 잡아서 "배치 진입 시각 기준(버그)"과 "slot1 실제
    // 전송 시각 기준(정상)" 사이 폭에서 둘의 타임아웃 판정이 갈리게 한다.
    OtaSession session(transport, /*batchSize=*/2, /*timeoutMs=*/50, /*maxRetry=*/5,
                        /*chunkDelayMs=*/40);

    session.start(path, 1, kSessionId, /*nowMs=*/1000);
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));

    session.tick(1010); // Handshaking -> SendingBatch(slot0@1010, slot1@~1050) -> WaitingBatchAck
    assert(session.state() == OtaSessionState::WaitingBatchAck);
    assert(transport.countSentOfType(OTA_PKT_DATA) == 2); // seq0, seq1 최초 전송만

    // slot0(seq0)만 ACK
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));
    session.tick(1050);
    assert(session.progress().ackedChunks == 1);

    // slot1(seq1)은 응답 없음. nowMs=1080: slot1의 "실제" 전송 시각(~1050)
    // 기준으로는 30ms만 지나 타임아웃(50ms) 전이지만, 옛 버그의 "배치 진입
    // 시각(1010)" 기준으로는 70ms 지나 이미 타임아웃이었을 시점.
    session.tick(1080);
    assert(session.state() == OtaSessionState::WaitingBatchAck);
    assert(transport.countSentOfType(OTA_PKT_DATA) == 2); // 아직 재전송 없어야 함(수정 후)

    // 이제 정상적으로 응답 -> 완료 진행 확인(잔여 로직 안 깨졌는지)
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 1));
    session.tick(1090);
    assert(session.state() == OtaSessionState::WaitingEndAck);

    std::remove(path.c_str());
    std::cout << "[OK] batchSlotsGetIndividualSentAtMsNotSharedBatchStart\n";
}

// [버그 수정 2026-08-20, ESP32 담당자 리포트 클레임 1] START ACK를 기다리는
// 중에 sessionId/sequence(OTA_CONTROL_SEQUENCE)는 맞지만 acknowledged_type이
// START가 아닌(END인 척하는) ACK가 와도 이걸 START 응답으로 착각해서 넘어가면
// 안 된다 — OTA_CONTROL_SEQUENCE 값만으로는 START/END 응답을 구분할 수 없어서
// acknowledged_type까지 같이 확인해야 한다.
void handshakeIgnoresAckWithWrongAcknowledgedType()
{
    const std::string path = writeTempFile("os_wrongtype1.bin", repeat('L', 10)); // 1청크
    constexpr uint32_t kSessionId = 0xCCCCCCCCu;

    FakeTransport transport;
    OtaSession session(transport, 1, /*timeoutMs=*/300, /*maxRetry=*/5, 0);

    session.start(path, 1, kSessionId, 1000);
    assert(session.state() == OtaSessionState::Handshaking);

    // sessionId/sequence는 맞지만 acknowledged_type이 END인 "가짜" START ACK
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_END, OTA_CONTROL_SEQUENCE));
    session.tick(1010);
    assert(session.state() == OtaSessionState::Handshaking); // 아직 안 넘어가야 함

    // 진짜 START ACK가 오면 정상적으로 다음 단계로
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1020);
    assert(session.state() == OtaSessionState::WaitingBatchAck);

    std::remove(path.c_str());
    std::cout << "[OK] handshakeIgnoresAckWithWrongAcknowledgedType\n";
}

// pollAndApplyAckOrNack()에서도 마찬가지 — acknowledged_type이 DATA가 아니면
// sequence 값이 우연히 배치 슬롯의 sequence와 같아도 그 슬롯에 매칭하면 안 됨.
void batchIgnoresAckWithWrongAcknowledgedType()
{
    const std::string path = writeTempFile("os_wrongtype2.bin", repeat('M', 10)); // 1청크
    constexpr uint32_t kSessionId = 0xDDDDDDDDu;

    FakeTransport transport;
    OtaSession session(transport, 1, /*timeoutMs=*/300, /*maxRetry=*/5, 0);

    session.start(path, 1, kSessionId, 1000);
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010); // DATA(seq0) 전송, WaitingBatchAck
    assert(session.state() == OtaSessionState::WaitingBatchAck);

    // sequence는 seq0(0)과 우연히 같지만 acknowledged_type이 START인 "가짜" ACK
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, 0));
    session.tick(1020);
    assert(session.progress().ackedChunks == 0); // 매칭되면 안 됨

    // 진짜 DATA ACK가 오면 정상 반영
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));
    session.tick(1030);
    assert(session.progress().ackedChunks == 1);

    std::remove(path.c_str());
    std::cout << "[OK] batchIgnoresAckWithWrongAcknowledgedType\n";
}

// [2026-08-20 추가, "Gateway ACK/NACK 로그 추가" 요청 대응] setOnLog()로 구독한
// 콜백이 ACK/NACK/타임아웃 이벤트마다 실제로 호출되는지 확인. 메시지 문자열
// 자체를 엄격히 검증하기보다("seq=0" 같은 부분 문자열만 확인), 콜백이 최소
// 한 번은 의미 있게 불렸는지 + 관련 정보(sequence)가 담겼는지만 가볍게 확인.
void logCallbackFiresOnNackAndAck()
{
    const std::string path = writeTempFile("os_logcb.bin", repeat('N', 10)); // 1청크
    constexpr uint32_t kSessionId = 0xEEEEEEEEu;

    FakeTransport transport;
    OtaSession session(transport, 1, /*timeoutMs=*/300, /*maxRetry=*/5, 0);

    std::vector<std::string> logs;
    session.setOnLog([&logs](const std::string &msg) { logs.push_back(msg); });

    session.start(path, 1, kSessionId, 1000);
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010); // START ACK -> 로그 1건 이상 기대
    assert(!logs.empty());

    // seq0 NACK -> 재전송 로그가 찍히는지 ("seq=0"과 "NACK"이 메시지 어딘가에 있어야 함)
    logs.clear();
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_NACK, kSessionId, OTA_PKT_DATA, 0, OTA_RESULT_INVALID_CRC));
    session.tick(1020);
    bool sawNackLog = false;
    for (const auto &line : logs) {
        if (line.find("seq=0") != std::string::npos && line.find("NACK") != std::string::npos)
            sawNackLog = true;
    }
    assert(sawNackLog);

    std::remove(path.c_str());
    std::cout << "[OK] logCallbackFiresOnNackAndAck\n";
}

// [2026-08-20 추가, 실기기 재현 버그(design-notes 37절, seq=959) 대응]
// 큐에 낡은 NACK과 그 뒤를 바로 잇는 최신 ACK이 "같은 순간에 이미 같이"
// 쌓여 있으면, 예전엔 한 틱에 하나만 처리해서 ACK을 보려면 tick()을 한 번
// 더 불러야 했다. drainAckOrNackQueue()로 고친 뒤에는 같은 tick() 한 번
// 안에서 NACK(재전송 유발) 처리 -> 곧바로 ACK(acked=true) 처리까지 이어져야
// 한다 — "완전히 해결"은 아니고(낡은 응답이 재시도 한도보다 많이 쌓이면
// 여전히 실패할 수 있음, design-notes 37절 참고) "같은 틱 안에 이미 도착해
// 있는 최신 정보를 더 빨리 보게 됐다"는 개선을 검증하는 테스트.
void drainQueueSeesFreshAckInSameTickAsStaleNack()
{
    const std::string path = writeTempFile("os_drain.bin", repeat('O', 10)); // 1청크
    constexpr uint32_t kSessionId = 0xFFFFFFFEu;

    FakeTransport transport;
    OtaSession session(transport, 1, /*timeoutMs=*/300, /*maxRetry=*/5, 0);

    session.start(path, 1, kSessionId, 1000);
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_START, OTA_CONTROL_SEQUENCE));
    session.tick(1010); // DATA(seq0) 최초 전송 -> WaitingBatchAck
    assert(transport.countSentOfType(OTA_PKT_DATA) == 1);

    // seq0에 대한 "낡은" NACK과 그 직후의 "최신" ACK을 미리 같이 큐에 넣어둠
    // — 실기기에서 CC1101 수신 버퍼에 두 응답이 함께 쌓여 있던 상황을 흉내.
    transport.rxQueue.push_back(
        makeAckOrNack(OTA_PKT_NACK, kSessionId, OTA_PKT_DATA, 0, OTA_RESULT_INVALID_CRC));
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, kSessionId, OTA_PKT_DATA, 0));

    session.tick(1020); // 단 한 번의 tick() 안에서 NACK 처리(재전송)와 ACK 처리(acked)가 다 끝나야 함
    assert(transport.countSentOfType(OTA_PKT_DATA) == 2); // NACK으로 인한 재전송 1회
    assert(session.progress().ackedChunks == 1); // 같은 틱 안에서 ACK까지 반영됨
    assert(session.state() == OtaSessionState::WaitingEndAck); // 1청크뿐이라 배치 완료 -> END로

    std::remove(path.c_str());
    std::cout << "[OK] drainQueueSeesFreshAckInSameTickAsStaleNack\n";
}

} // namespace

int main()
{
    happyPathCompletesAcrossMultipleBatches();
    nackRetransmitsOnlyThatChunkImmediately();
    timeoutTriggersResendOfSameChunk();
    batchMaxRetryExceededLeadsToFailed();
    handshakeMaxRetryExceededLeadsToFailed();
    endNackGoesDirectlyToFailedWithoutRetry();
    resumeDoesNotCauseSpuriousTimeout();
    batchSendingPollsAlreadyArrivedAckDuringSend();
    retransmitSlotPollsForResponseDuringItsOwnDelay();
    batchSlotsGetIndividualSentAtMsNotSharedBatchStart();
    handshakeIgnoresAckWithWrongAcknowledgedType();
    batchIgnoresAckWithWrongAcknowledgedType();
    logCallbackFiresOnNackAndAck();
    drainQueueSeesFreshAckInSameTickAsStaleNack();
    std::cout << "\n모든 테스트 통과\n";
    return 0;
}
