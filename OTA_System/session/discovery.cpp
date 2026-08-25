#include "discovery.h"

#include "cc1101transport.h"
#include "itransport.h"
#include "simplereceiver.h"

extern "C" {
#include "ota_protocol.h"
}

#include <algorithm>
#include <chrono>
#include <thread>

namespace {

// [2026-08-25] DISCOVER 전 FHSS 잔류 상태 초기화 — 이 함수 하나로 모으기 전엔
// smoke_discover_main.cpp(CLI, 2026-08-24 6ec6a34)에만 이 리셋이 있었고,
// otamanager.cpp(Qt 화면)는 같은 문제를 그대로 갖고 있었다(실기기 재현:
// ESP32가 MENU_OTA에서 168초 동안 패킷을 하나도 못 받음 — DISCOVER가 이전
// FHSS 세션이 남긴 호핑 상태 때문에 계속 엉뚱한 채널로 나갔다). "CLI로
// 검증한 로직을 화면도 그대로 쓴다"는 원칙이 지켜지려면 이런 리셋은 각
// 호출부(CLI main(), Qt 슬롯)가 각자 기억해서 넣는 게 아니라 discoverDevices()
// 자신이 항상 하는 게 맞다 — 그래서 여기로 옮겼다. 앞으로 새 호출부가
// 생겨도(예: 다른 CLI 도구) 이 함수만 부르면 자동으로 안전하다.
//
// ITransport는 CC1101 전용 메서드(stopFhss 등)를 모르므로(의도적 추상화 —
// discovery.h 상단 주석 참고) dynamic_cast로 "혹시 CC1101이면" 리셋하고,
// 아니면(FakeTransport 등 테스트용) 조용히 건너뛴다 — tst_discovery.cpp가
// 실기기 없이 그대로 통과하는 이유.
void resetLeftoverFhssStateIfCc1101(ITransport &transport)
{
    auto *cc1101 = dynamic_cast<Cc1101Transport *>(&transport);
    if (cc1101 == nullptr)
        return; // FakeTransport 등 — CC1101이 아니면 리셋할 잔류 상태 자체가 없음

    // 실패해도 최대한 진행한다(discoverDevices()는 성공/실패를 vector 크기로만
    // 알리는 단순한 API라, 여기서 에러를 전파하지 않음 — 리셋이 실패해도
    // DISCOVER 자체는 시도해볼 가치가 있음. 상세 실패 원인이 필요하면 호출부가
    // transport.lastStatus()로 직접 확인 가능).
    (void)cc1101->stopFhss();
    (void)cc1101->setChannel(0);
    cc1101->flushRx();
    (void)cc1101->startRx();
}

} // namespace

std::vector<DiscoveredDevice> discoverDevices(ITransport &transport, int waitMs)
{
    std::vector<DiscoveredDevice> found;

    resetLeftoverFhssStateIfCc1101(transport);

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
