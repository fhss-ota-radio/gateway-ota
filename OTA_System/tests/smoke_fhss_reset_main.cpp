// 실기기(CC1101) 스모크테스트 — 잔여 FHSS 호핑 상태를 정리하고 채널 0
// 고정 상태로 되돌리기만 하는 최소 CLI.
//
// [왜 필요한가] ota_smoke_session_send(비호핑 OtaSession 전송)는
// transport.open()/startRx()만 부르고 stopFhss()/setChannel(0)/flushRx()는
// 안 부른다(smoke_session_send_main.cpp 참고) — 그래서 이전 실행이 커널
// FHSS MASTER를 호핑 상태로 남겨뒀으면, ESP는 채널 0에서 정상 대기 중인데
// Gateway만 딴 채널(1~8)을 계속 도는 어긋남이 생긴다(148 실기기 사례,
// design-notes-gateway-ota-es.md 73절). ota_smoke_fhss_activate/
// ota_smoke_fhss_ota_transfer는 이 정리를 자기 흐름의 1단계로 포함하고
// 있지만, 그 뒤에 CONFIG/ACTIVATE 핸드셰이크까지 이어져서 "그냥 정리만
// 하고 끝내기" 용도로 쓰기엔 안 맞는다(중간에 죽이면 어중간한 상태가 됨).
// 이 파일은 딱 그 정리 단계만 떼어내 재사용 가능하게 만든 것 — 비호핑
// 테스트(ota_smoke_session_send) 직전에 한 번 실행해서 항상 깨끗한 상태
// (FHSS OFF, 채널 0)에서 시작하게 하는 용도.
//
// 사용법: ota_smoke_fhss_reset <device_path>
// 예:    ota_smoke_fhss_reset /dev/cc1101

#include "cc1101transport.h"
#include "teelogger.h"

#include <iostream>
#include <string>

int main(int argc, char *argv[])
{
    TeeLogger logger("gw_log");

    if (argc < 2) {
        std::cerr << "사용법: " << argv[0] << " <device_path>\n"
                  << "예: " << argv[0] << " /dev/cc1101\n";
        return 1;
    }
    const std::string devicePath = argv[1];

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "[fhss_reset] transport open 실패: " << devicePath << "\n";
        return 1;
    }

    // [2026-08-26] smoke_fhss_activate_main.cpp / smoke_fhss_ota_transfer_main.cpp
    // 1단계와 완전히 동일한 순서·이유였던 걸 Cc1101Transport::resetToFixedChannel()
    // 하나로 통합함(cc1101transport.h 주석 참고) — 실기기로 이미 검증된 정리
    // 절차를 그대로 재사용(새 로직 아님), 이제 이 파일도 그 공용 구현을 부르기만 함.
    const bool ok = transport.resetToFixedChannel([](const std::string &msg) {
        std::cerr << "[fhss_reset] " << msg << "\n";
    });

    transport.close();

    if (ok) {
        std::cout << "[fhss_reset] 완료 — CC1101이 채널 0, 비호핑 상태로 정리됨. "
                     "이제 ota_smoke_session_send를 실행하세요.\n";
        return 0;
    }
    std::cerr << "[fhss_reset] 일부 단계 실패 — 위 로그를 확인하세요.\n";
    return 1;
}
