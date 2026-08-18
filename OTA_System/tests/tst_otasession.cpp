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
    std::cout << "\n모든 테스트 통과\n";
    return 0;
}
