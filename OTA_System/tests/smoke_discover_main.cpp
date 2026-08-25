// 실기기(CC1101) 스모크테스트 — session/discovery.h의 discoverDevices()로
// 실제 ESP32를 조회해보는 CLI.
//
// [왜 필요한가] tests/tst_discovery.cpp는 FakeTransport(인메모리)로 로직만
// 검증한 것이고, 진짜 CC1101 RF 링크로 OTA_DISCOVER를 브로드캐스트했을 때
// firmware-esp32의 ota_consumer_handle_discover()가 실제로 DISCOVER_ACK를
// 응답하는지는 실기기로 확인해야 한다. ESP32는 MENU_OTA 화면에 있을 때만
// 응답하므로(ota_consumer_is_ota_mode() 조건), 실행 전에 ESP32를 그 상태로
// 맞춰둘 것.
//
// 사용법:
//   ota_smoke_discover <device_path> [wait_ms]
//
//   wait_ms 기본값 1000 — discoverDevices()의 기본값과 동일.
//
// device_id는 ota_protocol.h 453행 주석의 표시 관례대로 "AA-BB-CC"
// (hex 3그룹, MAC 뒤 3byte)로 출력한다.

#include "cc1101transport.h"
#include "discovery.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cerr << "사용법: " << argv[0] << " <device_path> [wait_ms]\n";
        return 1;
    }

    const std::string devicePath = argv[1];
    int waitMs = 1000;
    if (argc >= 3) {
        waitMs = std::atoi(argv[2]);
        if (waitMs <= 0) {
            std::cerr << "wait_ms는 양의 정수여야 합니다: " << argv[2] << "\n";
            return 1;
        }
    }

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "transport open 실패: " << devicePath << "\n";
        return 1;
    }

    // [2026-08-24 추가, 2026-08-25 discoverDevices()로 이동] FHSS 잔류 상태
    // (이전 실행이 stopFhss() 없이 끝나서 커널 드라이버가 여전히 호핑 중인
    // 상태) 정리는 예전엔 여기서 직접 했지만, 지금은 discoverDevices()가
    // (CC1101이면) 항상 알아서 해준다 — otamanager.cpp(Qt 화면)가 이 리셋을
    // 안 하고 있던 문제가 실기기로 발견돼서, 각 호출부가 따로 챙기는 대신
    // discoverDevices() 하나로 모았다(session/discovery.cpp
    // resetLeftoverFhssStateIfCc1101() 참고). 이 CLI는 startRx()를 별도로
    // 부를 필요도 없어졌다.

    std::cout << "[discover] OTA_DISCOVER 브로드캐스트 전송, " << waitMs
               << "ms 동안 응답 대기...\n";
    std::cout << "[discover] (ESP32가 MENU_OTA 화면에 있어야 응답합니다)\n";

    const auto found = discoverDevices(transport, waitMs);

    if (found.empty()) {
        std::cout << "[discover] 응답 없음 — ESP32가 MENU_OTA 상태인지, "
                   << "주파수/싱크워드가 맞는지 확인하세요.\n";
    } else {
        std::cout << "[discover] " << found.size() << "대 발견:\n";
        for (const auto &device : found) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%02X-%02X-%02X",
                          static_cast<unsigned>((device.deviceId >> 16) & 0xFFu),
                          static_cast<unsigned>((device.deviceId >> 8) & 0xFFu),
                          static_cast<unsigned>(device.deviceId & 0xFFu));
            std::cout << "  - device_id=" << buf << " fw=" << static_cast<int>(device.fwMajor)
                       << "." << static_cast<int>(device.fwMinor) << "."
                       << static_cast<int>(device.fwPatch) << "\n";
        }
    }

    transport.close();
    std::cout << "[discover] 종료\n";
    return 0;
}
