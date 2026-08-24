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

    // [2026-08-24 추가] DISCOVER/FHSS_CONFIG/FHSS_ACTIVATE는 호핑 시작 전
    // "부트스트랩 채널 0"에서 주고받는다. 그런데 FHSS 호핑은 이 프로세스가
    // 끝나도 커널 드라이버 레벨에서 계속 돈다 — smoke_fhss_activate_main.cpp
    // 127행 주석과 동일한 이유로, startFhss()를 부른 이전 실행(예:
    // ota_smoke_fhss_ota_transfer)이 stopFhss() 없이 죽거나 그냥 끝나면
    // 칩은 여전히 1~8번 채널을 계속 호핑 중인 채로 남는다. 그 상태에서
    // 이 도구가 startRx()만 부르고 DISCOVER를 쏘면, ESP32(MENU_OTA, 채널
    // 0 고정)와 Gateway가 같은 채널에 있는 짧은 순간에만 우연히 만나므로
    // 사실상 "응답 없음"만 계속 나온다 — 148/149 실기기에서 이 증상으로
    // 재현됨(팀원 리뷰로 원인 확인, 2026-08-24). smoke_fhss_activate_main.cpp
    // 145~165행처럼 매번 명시적으로 정지+채널0 고정+FIFO 비우기를 하고,
    // 여기 실패는 원인을 숨기지 않도록 그냥 진행하지 않고 종료한다.
    const Cc1101Status stopStatus = transport.stopFhss();
    if (stopStatus != Cc1101Status::Ok) {
        std::cerr << "stopFhss 실패 (코드=" << static_cast<int>(stopStatus)
                   << ") — 커널 드라이버가 호핑 중일 수 있는데 정지가 안 됨\n";
        return 1;
    }

    const Cc1101Status channelStatus = transport.setChannel(0);
    if (channelStatus != Cc1101Status::Ok) {
        std::cerr << "setChannel(0) 실패 (코드=" << static_cast<int>(channelStatus)
                   << ")\n";
        return 1;
    }

    // 이전 세션이 남긴 패킷이나 RXFIFO_OVERFLOW 상태가 섞여 들어오지
    // 않도록 FIFO를 비운 뒤 RX로 재진입한다. flushRx()는 FLUSH_RX ioctl만
    // 하고 RX 재진입은 안 해주므로 startRx()를 별도로 불러야 한다
    // (cc1101transport.cpp 209~222행 참고).
    transport.flushRx();
    if (transport.lastStatus() != Cc1101Status::Ok) {
        std::cerr << "flushRx 실패 (코드=" << static_cast<int>(transport.lastStatus())
                   << ")\n";
        return 1;
    }

    if (transport.startRx() != Cc1101Status::Ok) {
        std::cerr << "startRx 실패 (코드=" << static_cast<int>(transport.lastStatus())
                   << ")\n";
        return 1;
    }

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
