#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <cstdint>
#include <vector>

class ITransport;

// OTA_DISCOVER/OTA_DISCOVER_ACK 기반 기기 탐색.
//
// [의도적으로 OtaSession 밖] session/otasession.h의 클래스 주석 그대로 —
// DISCONNECTED/CONNECTED_IDLE/FILE_READY/DISCOVERING/SELECTING은 "세션이
// 시작되기 전" 화면(또는 CLI)이 담당할 상위 흐름이라 OtaSession에 안 넣기로
// 했었다. 이 파일이 그 DISCOVERING/SELECTING 단계를 구현한다 — session_id가
// 아직 없는 시점(ota_protocol.h "왜 DISCOVER에 session_id가 없는지" 참고)의
// 별도 왕복이라 OtaSession의 세션 개념과 자연스럽게 분리된다.
//
// [의도적으로 Qt 의존성 없음] core/·transport/·session/의 다른 파일과 같은
// 이유 — g++/clang++만으로 컴파일·테스트 가능해야 tests/tst_discovery.cpp가
// 실기기 없이도 로직만 검증할 수 있음.

// 조회에 응답한 기기 하나. ota-protocol의 ota_discover_ack_fields_t와
// 대응하지만, 이 파일이 ota_protocol.h 타입에 직접 의존하지 않도록(다른
// session/*.h 파일들과 같은 원칙) 별도 struct로 둠.
struct DiscoveredDevice
{
    uint32_t deviceId = 0;   // MAC 뒤 3byte (ota_protocol.h 참고), 0 ~ OTA_DEVICE_ID_MAX
    uint8_t fwMajor = 0;
    uint8_t fwMinor = 0;
    uint8_t fwPatch = 0;
};

// OTA_DISCOVER를 한 번 브로드캐스트로 보내고, waitMs 동안 도착하는
// DISCOVER_ACK들을 모아서 반환한다(블로킹 — Qt 화면에서 쓸 때는 이 호출
// 자체를 별도 스레드로 돌리거나, 나중에 논블로킹 버전을 따로 만들 것).
//
// 같은 device_id가 여러 번 응답해도(예: 재전송이나 RF 노이즈로 인한 중복)
// 한 번만 반환한다 — 나중 응답으로 덮어씀(펌웨어 버전이 그 사이 바뀌는
// 경우는 실질적으로 없지만, 최신 값을 우선한다는 원칙).
//
// waitMs: 여러 기기가 동시에 응답하면 무선 충돌이 날 수 있어서 ESP32
// 쪽에서 짧은 랜덤 백오프를 두게 돼 있음(ota_protocol.h 주석 참고) —
// 그래서 1회 왕복보다 넉넉한 대기 시간이 필요하다. 기본값 1000ms.
std::vector<DiscoveredDevice> discoverDevices(ITransport &transport, int waitMs = 1000);

#endif // DISCOVERY_H
