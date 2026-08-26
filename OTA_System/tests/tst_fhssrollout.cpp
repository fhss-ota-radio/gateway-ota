/*
 * session/fhssrollout.h(rolloutFhssConfig()) 유닛테스트. tst_discovery.cpp와
 * 같은 assert 기반 스타일 + FakeTransport, Qt 의존성 없음.
 *
 * rolloutFhssConfig()는 실제 시각(steady_clock)으로 timeoutMs만큼 기다리는
 * 함수라, 여기서는 timeoutMs/maxRetry를 작게 잡아서 테스트를 빠르게
 * 유지한다(discoverDevices() 테스트와 같은 타협).
 *
 *   g++ -std=c++17 -Wall -Wextra \
 *       -I.. -I../core -I../transport -I../session -I../../../ota-protocol/include \
 *       tst_fhssrollout.cpp ../session/fhssrollout.cpp ../session/simplereceiver.cpp \
 *       -o tst_fhssrollout
 *   ./tst_fhssrollout
 */
#include "fhssrollout.h"

#include "itransport.h"

#include <cassert>
#include <deque>
#include <iostream>
#include <vector>

extern "C" {
#include "ota_protocol.h"
}

namespace {

// tst_discovery.cpp의 FakeTransport와 같은 역할.
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

std::vector<uint8_t> makeAckOrNack(ota_packet_type_t type, uint32_t sessionId,
                                    ota_packet_type_t acknowledgedType,
                                    uint8_t resultCode = OTA_RESULT_OK)
{
    ota_ack_fields_t fields{};
    fields.session_id = sessionId;
    fields.acknowledged_type = static_cast<uint8_t>(acknowledgedType);
    fields.sequence = OTA_CONTROL_SEQUENCE;
    fields.result_code = resultCode;

    uint8_t buf[OTA_ACK_PACKET_SIZE];
    const size_t written = ota_protocol_encode_ack(buf, sizeof(buf), type, &fields);
    assert(written == OTA_ACK_PACKET_SIZE);
    return std::vector<uint8_t>(buf, buf + written);
}

FhssHopPolicy samplePolicy()
{
    FhssHopPolicy policy;
    policy.generation = 42;
    policy.algorithmVersion = OTA_FHSS_ALGORITHM_VERSION;
    policy.channelProfileId = 1;
    policy.firstChannel = 1;
    policy.channelCount = 5;
    policy.rendezvousChannel = 1; // ota_fhss_config_is_valid() 조건: rendezvous == first
    policy.reservedChannel = 0;
    policy.seed = 0x1234;
    policy.slotDurationUs = 300000;
    policy.channelSwitchGuardUs = 5000;
    return policy;
}

// 기기 1대, CONFIG ACK과 ACTIVATE ACK이 둘 다 즉시 오면 Activated까지 가는지.
void rolloutSucceedsThroughActivateWhenBothAcksArrive()
{
    FakeTransport transport;
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, 100, OTA_PKT_FHSS_CONFIG));
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, 100, OTA_PKT_FHSS_ACTIVATE));

    const auto outcomes = rolloutFhssConfig(transport, /*sessionId=*/100, {0xAAAAAAu},
                                             samplePolicy(), /*timeoutMs=*/20, /*maxRetry=*/1);

    assert(outcomes.size() == 1);
    assert(outcomes[0].deviceId == 0xAAAAAAu);
    assert(outcomes[0].stage == FhssRolloutStage::Activated);
    assert(transport.countSentOfType(OTA_PKT_FHSS_CONFIG) == 1);
    assert(transport.countSentOfType(OTA_PKT_FHSS_ACTIVATE) == 1);

    std::cout << "[OK] rolloutSucceedsThroughActivateWhenBothAcksArrive\n";
}

// 응답이 하나도 없으면 재시도(maxRetry=1 -> 2번 전송) 후 ConfigFailed로 끝나는지.
void rolloutFailsAfterRetriesWhenNoResponse()
{
    FakeTransport transport; // rxQueue 비워둠 — 계속 무응답

    const auto outcomes = rolloutFhssConfig(transport, /*sessionId=*/200, {0xBBBBBBu},
                                             samplePolicy(), /*timeoutMs=*/20, /*maxRetry=*/1);

    assert(outcomes.size() == 1);
    assert(outcomes[0].stage == FhssRolloutStage::ConfigFailed);
    // maxRetry=1이면 최초 1회 + 재시도 1회 = 총 2번 CONFIG를 보내야 한다.
    assert(transport.countSentOfType(OTA_PKT_FHSS_CONFIG) == 2);
    assert(transport.countSentOfType(OTA_PKT_FHSS_ACTIVATE) == 0); // CONFIG 실패했으니 ACTIVATE는 아예 안 나감

    std::cout << "[OK] rolloutFailsAfterRetriesWhenNoResponse\n";
}

// 기기 A는 끝까지 성공, 기기 B는 CONFIG부터 계속 NACK — 한 기기의 실패가
// 다른 기기 처리를 막지 않는지, 그리고 1단계(CONFIG 전부)를 다 끝낸
// 뒤에야 2단계(ACTIVATE)로 넘어가는지(순서) 확인.
//
// [FakeTransport 큐 순서 주의] rxQueue는 poll 호출 순서대로(FIFO) 소비된다 —
// "이 응답은 이 시점에야 온다"는 타이밍 개념이 없다. 그래서 B가 시간
//초과(무응답)로 실패하게 하면, 그 무응답 대기 구간에 아직 큐에 남아있는
// (A용) ACTIVATE ACK을 B의 폴링이 먼저 먹어버려 테스트가 불안정해진다.
// 그 문제를 피하려고 B는 매 시도마다 즉시 NACK을 받게 해서(무응답 대기
// 없이 바로 다음 시도로 넘어가게) 큐 소비 순서를 완전히 결정적으로 만든다.
void rolloutIsolatesFailureAndFinishesAllConfigsBeforeAnyActivate()
{
    FakeTransport transport;
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, 300, OTA_PKT_FHSS_CONFIG));  // A attempt0 -> 성공
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_NACK, 300, OTA_PKT_FHSS_CONFIG)); // B attempt0 -> NACK, 즉시 재시도
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_NACK, 300, OTA_PKT_FHSS_CONFIG)); // B attempt1(마지막) -> NACK, 실패 확정
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, 300, OTA_PKT_FHSS_ACTIVATE)); // A의 ACTIVATE (2단계)

    const auto outcomes = rolloutFhssConfig(transport, /*sessionId=*/300,
                                             {0x0A0A0Au, 0x0B0B0Bu}, samplePolicy(),
                                             /*timeoutMs=*/20, /*maxRetry=*/1);

    assert(outcomes.size() == 2);
    assert(outcomes[0].deviceId == 0x0A0A0Au);
    assert(outcomes[0].stage == FhssRolloutStage::Activated);
    assert(outcomes[1].deviceId == 0x0B0B0Bu);
    assert(outcomes[1].stage == FhssRolloutStage::ConfigFailed);

    // 순서 확인: A CONFIG, B CONFIG(x2, 재시도 포함)까지 전부 나간 뒤에야
    // A ACTIVATE가 나가야 한다 — 즉 마지막으로 보낸 패킷이 ACTIVATE.
    assert(!transport.sentPackets.empty());
    assert(transport.sentPackets.back()[0] == static_cast<uint8_t>(OTA_PKT_FHSS_ACTIVATE));

    std::cout << "[OK] rolloutIsolatesFailureAndFinishesAllConfigsBeforeAnyActivate\n";
}

// CONFIG에 대해 NACK이 오면(재시도로 취급) 다음 시도에서 ACK이 오면 성공하는지.
void rolloutRetriesAfterNackAndSucceedsOnNextAttempt()
{
    FakeTransport transport;
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_NACK, 400, OTA_PKT_FHSS_CONFIG));
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, 400, OTA_PKT_FHSS_CONFIG));
    transport.rxQueue.push_back(makeAckOrNack(OTA_PKT_ACK, 400, OTA_PKT_FHSS_ACTIVATE));

    const auto outcomes = rolloutFhssConfig(transport, /*sessionId=*/400, {0xCCCCCCu},
                                             samplePolicy(), /*timeoutMs=*/20, /*maxRetry=*/2);

    assert(outcomes.size() == 1);
    assert(outcomes[0].stage == FhssRolloutStage::Activated);
    assert(transport.countSentOfType(OTA_PKT_FHSS_CONFIG) == 2); // NACK 받고 1번 더 보냄

    std::cout << "[OK] rolloutRetriesAfterNackAndSucceedsOnNextAttempt\n";
}

} // namespace

int main()
{
    rolloutSucceedsThroughActivateWhenBothAcksArrive();
    rolloutFailsAfterRetriesWhenNoResponse();
    rolloutIsolatesFailureAndFinishesAllConfigsBeforeAnyActivate();
    rolloutRetriesAfterNackAndSucceedsOnNextAttempt();
    std::cout << "\n모든 테스트 통과\n";
    return 0;
}
