#ifndef FHSSROLLOUT_H
#define FHSSROLLOUT_H

#include <cstdint>
#include <vector>

class ITransport;

// [설계 배경, 2026-08-22] gateway-ota 담당 "3. ESP32 설정 배포" +
// "4. 모든 대상의 READY ACK 확인 후 활성화" 작업.
//
// firmware-esp32 develop(725b27d, feat(fsm): model OTA FHSS synchronization
// lifecycle)에서 확인한 ESP32 FSM 순서를 그대로 따른다:
//   MENU_OTA --(FHSS_CONFIG 받고 ACK)--> OTA_FHSS_CONFIGURED
//           --(FHSS_ACTIVATE 받고 ACK)--> OTA_FHSS_SYNCING (5초 내 동기화 필요)
//           --(동기화 성공, SYNC_ACQUIRED)--> OTA_FHSS_READY (이제야 OTA_START를 받아줌)
// 이 파일은 그중 CONFIG/ACTIVATE 왕복만 담당한다. SYNC 자체(실제로 주파수를
// 맞춰가는 것)는 Cc1101Transport::startFhss() 이후 커널이 자동/오토너머스로
// 하는 일이라(kernel-cc1101-spi "add autonomous FHSS synchronization and
// recovery") 여기서 손댈 일이 아니다 — discovery.h가 DISCOVERING을 OtaSession
// 밖에 둔 것과 같은 원칙으로, 이 롤아웃도 OtaSession 밖의 독립 함수로 둔다.
//
// [READY ACK == 새 패킷 아님] firmware-esp32의 ota_consumer.c를 확인한 결과,
// FHSS_CONFIG/FHSS_ACTIVATE에 대한 응답은 전용 READY 패킷이 아니라 기존
// OTA_PKT_ACK/NACK을 그대로 재사용한다(acknowledged_type으로 어떤 패킷에
// 대한 응답인지 구분 — Claim1에서 이미 만든 것과 같은 메커니즘). 그래서 이
// 파일은 simplereceiver.h의 ReceivedPacket/tryReceiveOnce()를 그대로 쓴다.
//
// [기기 여러 대를 왜 한 번에 하나씩만 처리하는가] ota_ack_fields_t(ACK/NACK
// 패킷)엔 "어느 기기가 보냈는지" 필드가 없다 — session_id+acknowledged_type+
// sequence만 있다. 그래서 여러 기기에 CONFIG를 동시에 뿌리면 어느 ACK이
// 어느 기기 것인지 구분할 방법이 없다. 대신 기기 하나씩 순서대로
// "보내고 그 기기 응답만 기다렸다가 다음 기기로 넘어가는" 방식으로
// 모호함 자체를 없앤다(discoverDevices()처럼 병렬로 모으는 방식이 아님 —
// DISCOVER_ACK는 device_id를 담고 있어서 병렬이 가능했지만 여긴 다름).

// FHSS_CONFIG에 실을 호핑 정책 값. Cc1101FhssConfig(transport/cc1101_status.h)의
// hop 부분과 값이 같아야 한다 — Gateway 커널도 이 값으로 호핑을 계산하고,
// ESP32도 같은 값을 받아 같은 순서를 계산해야 하기 때문. rf profile(기준
// 주파수 등 무선 레벨 값)은 여기 없음 — 그건 로컬(커널) 전용이라 애초에
// 와이어로 안 나간다(ota_protocol.h 설계와 동일한 원칙).
struct FhssHopPolicy
{
    uint32_t generation = 0;
    uint8_t  algorithmVersion = 1;   // OTA_FHSS_ALGORITHM_VERSION
    uint8_t  channelProfileId = 0;
    uint8_t  firstChannel = 1;       // 랑데부 채널과 같아야 함(프로토콜 검증 조건)
    uint8_t  channelCount = 0;
    uint8_t  rendezvousChannel = 1;
    uint8_t  reservedChannel = 0;    // OTA 전용(채널 0)은 호핑에서 제외
    uint32_t seed = 0;
    uint32_t slotDurationUs = 0;
    uint32_t channelSwitchGuardUs = 0;
};

// 기기 하나에 대한 롤아웃 결과 — 어느 단계까지 갔는지.
enum class FhssRolloutStage
{
    ConfigFailed = 0,  // CONFIG ACK을 끝내 못 받음(재시도 초과 또는 NACK)
    ConfigAcked,        // CONFIG는 성공했지만 ACTIVATE는 실패/미시도
    Activated,           // ACTIVATE ACK까지 받음 — 이 기기는 이제 OTA_FHSS_SYNCING으로 넘어갔을 것
};

struct FhssRolloutOutcome
{
    uint32_t deviceId = 0;
    FhssRolloutStage stage = FhssRolloutStage::ConfigFailed;
};

// targetDeviceIds를 하나씩 순서대로 처리한다: 그 기기에 FHSS_CONFIG를 보내고
// ACK을 기다리고(타임아웃/NACK 시 재시도, tickHandshaking()과 같은 패턴),
// 성공한 기기에 한해 FHSS_ACTIVATE까지 보낸다.
//
// "모든 대상의 READY ACK 확인 후 활성화" 요구사항은 이렇게 구현했다: 먼저
// 모든 기기의 CONFIG를 다 끝내고(1단계), CONFIG에 성공한 기기 목록이
// 확정된 뒤에야 그 기기들에게 ACTIVATE를 보낸다(2단계) — 한 기기라도 아직
// CONFIG 확인 전인데 다른 기기부터 ACTIVATE(=SYNCING 진입, 5초 타이머 시작)
// 해버리면 그 기기만 먼저 호핑을 시작해 나머지와 어긋날 수 있기 때문이다.
//
// 기기 하나가 CONFIG에 실패해도 나머지는 계속 진행한다(부분 실패를 전체
// 실패로 만들지 않음 — 실기기 테스트에서 기기 하나가 꺼져 있는 경우가
// 흔할 것이므로). 실패한 기기는 결과에 ConfigFailed로 남는다.
std::vector<FhssRolloutOutcome> rolloutFhssConfig(
    ITransport &transport, uint32_t sessionId,
    const std::vector<uint32_t> &targetDeviceIds, const FhssHopPolicy &policy,
    int timeoutMs = 300, int maxRetry = 5);

#endif // FHSSROLLOUT_H
