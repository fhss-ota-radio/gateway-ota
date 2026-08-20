#include "discovery.h"

#include "itransport.h"
#include "simplereceiver.h"

extern "C" {
#include "ota_protocol.h"
}

#include <algorithm>
#include <chrono>
#include <thread>

std::vector<DiscoveredDevice> discoverDevices(ITransport &transport, int waitMs)
{
    std::vector<DiscoveredDevice> found;

    uint8_t packet[OTA_DISCOVER_PACKET_SIZE];
    const size_t written = ota_protocol_encode_discover(packet, sizeof(packet));
    if (written == 0)
        return found; // 인코딩 자체가 실패하면(사실상 안 일어남) 빈 목록

    if (!transport.send(std::vector<uint8_t>(packet, packet + written)))
        return found;

    // 논블로킹 폴링을 waitMs 동안 반복 — OtaSession::enterSendingBatch()의
    // 폴링 루프(2026-08-19 효율 개선)와 같은 패턴. tryReceiveOnce()가
    // 즉시 리턴하는 함수라 계속 불러도 안전하다.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto received = tryReceiveOnce(transport);
        if (received.kind == ReceivedPacketKind::DiscoverAck) {
            auto it = std::find_if(found.begin(), found.end(),
                                    [&](const DiscoveredDevice &d) {
                                        return d.deviceId == received.deviceId;
                                    });
            DiscoveredDevice device;
            device.deviceId = received.deviceId;
            device.fwMajor = received.fwMajor;
            device.fwMinor = received.fwMinor;
            device.fwPatch = received.fwPatch;

            if (it != found.end())
                *it = device; // 나중 응답으로 덮어씀
            else
                found.push_back(device);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return found;
}
