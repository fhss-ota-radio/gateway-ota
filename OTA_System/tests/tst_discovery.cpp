/*
 * session/discovery.h(discoverDevices()) 유닛테스트. tst_otasession.cpp와
 * 같은 assert 기반 스타일, Qt 의존성 없음.
 *
 * discoverDevices()는 실제 시각(steady_clock)으로 waitMs만큼 기다리는
 * 함수라, 여기서는 waitMs를 짧게(수십 ms) 잡아서 테스트를 빠르게 유지한다
 * (session/otasession.cpp의 배치 전송 폴링 테스트와 같은 타협).
 *
 *   g++ -std=c++17 -Wall -Wextra \
 *       -I.. -I../core -I../transport -I../session -I../../../ota-protocol/include \
 *       tst_discovery.cpp ../session/discovery.cpp ../session/simplereceiver.cpp \
 *       -o tst_discovery
 *   ./tst_discovery
 */
#include "discovery.h"

#include "itransport.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>
#include <vector>

extern "C" {
#include "ota_protocol.h"
}

namespace {

// tst_otasession.cpp/tst_simplereceiver.cpp의 FakeTransport와 같은 역할.
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

std::vector<uint8_t> makeDiscoverAck(uint32_t deviceId, uint8_t major, uint8_t minor, uint8_t patch)
{
    ota_discover_ack_fields_t fields{};
    fields.device_id = deviceId;
    fields.fw_major = major;
    fields.fw_minor = minor;
    fields.fw_patch = patch;

    uint8_t buf[OTA_DISCOVER_ACK_PACKET_SIZE];
    const size_t written = ota_protocol_encode_discover_ack(buf, sizeof(buf), &fields);
    assert(written == OTA_DISCOVER_ACK_PACKET_SIZE);
    return std::vector<uint8_t>(buf, buf + written);
}

// 딱 한 번만 OTA_DISCOVER를 보내는지 (응답이 여러 개 와도 재조회는 안 해야 함).
void discoverDevicesSendsExactlyOneDiscoverPacket()
{
    FakeTransport transport;
    transport.rxQueue.push_back(makeDiscoverAck(0x010203u, 1, 0, 0));

    const auto found = discoverDevices(transport, /*waitMs=*/30);
    (void)found;

    assert(transport.countSentOfType(OTA_PKT_DISCOVER) == 1);
    std::cout << "[OK] discoverDevicesSendsExactlyOneDiscoverPacket\n";
}

// 서로 다른 기기 2대가 응답하면 둘 다 모이는지, 필드(버전)까지 정확히
// 담기는지 확인.
void discoverDevicesCollectsMultipleDistinctDevices()
{
    FakeTransport transport;
    transport.rxQueue.push_back(makeDiscoverAck(0xAABBCCu, 1, 2, 3));
    transport.rxQueue.push_back(makeDiscoverAck(0x112233u, 0, 9, 1));

    const auto found = discoverDevices(transport, /*waitMs=*/30);

    assert(found.size() == 2);
    auto it1 = std::find_if(found.begin(), found.end(),
                             [](const DiscoveredDevice &d) { return d.deviceId == 0xAABBCCu; });
    auto it2 = std::find_if(found.begin(), found.end(),
                             [](const DiscoveredDevice &d) { return d.deviceId == 0x112233u; });
    assert(it1 != found.end());
    assert(it2 != found.end());
    assert(it1->fwMajor == 1 && it1->fwMinor == 2 && it1->fwPatch == 3);
    assert(it2->fwMajor == 0 && it2->fwMinor == 9 && it2->fwPatch == 1);

    std::cout << "[OK] discoverDevicesCollectsMultipleDistinctDevices\n";
}

// 같은 device_id가 중복 응답하면 한 항목으로만 남는지(나중 값으로 덮어씀).
void discoverDevicesDeduplicatesSameDeviceId()
{
    FakeTransport transport;
    transport.rxQueue.push_back(makeDiscoverAck(0x555555u, 1, 0, 0));
    transport.rxQueue.push_back(makeDiscoverAck(0x555555u, 1, 1, 0)); // 같은 기기, 최신 버전

    const auto found = discoverDevices(transport, /*waitMs=*/30);

    assert(found.size() == 1);
    assert(found[0].deviceId == 0x555555u);
    assert(found[0].fwMinor == 1); // 나중 응답 값으로 덮어써짐

    std::cout << "[OK] discoverDevicesDeduplicatesSameDeviceId\n";
}

// 응답이 하나도 없으면 빈 목록(에러 아님).
void discoverDevicesReturnsEmptyWhenNoResponses()
{
    FakeTransport transport;
    const auto found = discoverDevices(transport, /*waitMs=*/30);
    assert(found.empty());
    assert(transport.countSentOfType(OTA_PKT_DISCOVER) == 1); // 그래도 질의는 나감

    std::cout << "[OK] discoverDevicesReturnsEmptyWhenNoResponses\n";
}

// DISCOVER_ACK이 아닌 다른 패킷(예: 엉뚱하게 섞여 들어온 ACK)은 무시하는지.
void discoverDevicesIgnoresNonDiscoverAckPackets()
{
    FakeTransport transport;

    ota_ack_fields_t ackFields{};
    ackFields.session_id = 1;
    ackFields.acknowledged_type = static_cast<uint8_t>(OTA_PKT_START);
    ackFields.sequence = OTA_CONTROL_SEQUENCE;
    ackFields.result_code = OTA_RESULT_OK;
    uint8_t ackBuf[OTA_ACK_PACKET_SIZE];
    ota_protocol_encode_ack(ackBuf, sizeof(ackBuf), OTA_PKT_ACK, &ackFields);
    transport.rxQueue.push_back(std::vector<uint8_t>(ackBuf, ackBuf + sizeof(ackBuf)));

    transport.rxQueue.push_back(makeDiscoverAck(0x777777u, 2, 0, 0));

    const auto found = discoverDevices(transport, /*waitMs=*/30);

    assert(found.size() == 1); // ACK은 무시되고 DISCOVER_ACK 하나만 담김
    assert(found[0].deviceId == 0x777777u);

    std::cout << "[OK] discoverDevicesIgnoresNonDiscoverAckPackets\n";
}

} // namespace

int main()
{
    discoverDevicesSendsExactlyOneDiscoverPacket();
    discoverDevicesCollectsMultipleDistinctDevices();
    discoverDevicesDeduplicatesSameDeviceId();
    discoverDevicesReturnsEmptyWhenNoResponses();
    discoverDevicesIgnoresNonDiscoverAckPackets();
    std::cout << "\n모든 테스트 통과\n";
    return 0;
}
