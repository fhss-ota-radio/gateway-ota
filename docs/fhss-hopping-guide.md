# FHSS(주파수 도약) 호핑 가이드

| 항목 | 내용 |
|---|---|
| 대상 레포 | `gateway-ota`(제어/롤아웃) + `kernel-cc1101-spi`(실제 호핑/SYNC) + `firmware-esp32`(SLAVE 쪽 대칭 구현) |
| 범위 | FHSS_CONFIG/ACTIVATE 배포 ~ 채널 호핑 순서 계산 ~ SYNC 동기화 상태 머신 |
| 최종 수정 | 2026-08-24 |
| 관련 브랜치 | `feature/gateway-fhss-sync`(`feature/gateway-qt-integration`에 병합 완료) |
| 관련 문서 | [`troubleshooting-fhss-hopping.md`](troubleshooting-fhss-hopping.md) — 이 기능을 만들며 겪은 증상별 문제 해결 |

## 1. 개요 — FHSS란, 왜 필요한가

FHSS(Frequency-Hopping Spread Spectrum, 주파수 도약 확산 스펙트럼)는 한
주파수에 고정해서 통신하지 않고, 양쪽이 미리 정한 규칙에 따라 짧은
간격(슬롯)마다 여러 채널을 옮겨 다니며 통신하는 방식입니다. 이 프로젝트는
OTA(펌웨어 전송)를 채널 0에 고정해서 쓰다가, 호핑을 얹어 **간섭 회피와
같은 대역 재사용**을 노립니다.

핵심 제약: **양쪽(Gateway=MASTER, ESP32=SLAVE)이 "정확히 같은 시각에
정확히 같은 채널"에 가 있어야만** 통신이 성립합니다. 이 문서는 그 규칙을
어떻게 만들고 맞추는지를 다룹니다.

## 2. 계층 구조

```
Gateway 애플리케이션 레벨 (gateway-ota)
├─ session/fhssrollout.h    FHSS_CONFIG/ACTIVATE를 무선으로 배포(3절)
└─ transport/cc1101transport.h
   configureFhss()/startFhss()/stopFhss()/getFhssStatus()   (4절)
        │ ioctl
        ▼
커널 레벨 (kernel-cc1101-spi)
├─ cc1101_fhss.c   SYNC 송수신, 상태 머신, 슬롯 타이머 (6절)
└─ cc1101_hop.c    seed로 채널 방문 순서를 만드는 알고리즘 (5절)
```

CONFIG/ACTIVATE 배포(3절)는 **채널 0**(OTA 전용 싱크워드)에서 오가는
"양방향 확인 대화"입니다. 실제 호핑과 SYNC 교환(5~6절)은 커널이 백그라운드
스레드로 자동 수행하는 "일방적 방송"에 가깝습니다 — 이 둘이 서로 다른
채널·다른 싱크워드를 쓴다는 걸 헷갈리면 안 됩니다
([`troubleshooting-fhss-hopping.md`](troubleshooting-fhss-hopping.md) 1장
참고 — 실제로 이걸로 하루를 태웠습니다).

## 3. FHSS_CONFIG/ACTIVATE 배포 — `session/fhssrollout.h`

```cpp
struct FhssHopPolicy
{
    uint32_t generation = 0;
    uint8_t  algorithmVersion = 1;
    uint8_t  channelProfileId = 0;
    uint8_t  firstChannel = 1;       // 랑데부 채널과 같아야 함
    uint8_t  channelCount = 0;
    uint8_t  rendezvousChannel = 1;
    uint8_t  reservedChannel = 0;    // OTA 전용(채널 0)은 호핑에서 제외
    uint32_t seed = 0;
    uint32_t slotDurationUs = 0;
    uint32_t channelSwitchGuardUs = 0;
};

std::vector<FhssRolloutOutcome> rolloutFhssConfig(
    ITransport &transport, uint32_t sessionId,
    const std::vector<uint32_t> &targetDeviceIds, const FhssHopPolicy &policy,
    int timeoutMs = 300, int maxRetry = 5, const FhssRolloutLogFn &onLog = nullptr);
```

`FhssHopPolicy`는 **무선으로 그대로 ESP32에 방송되는 값**입니다(RF
프로필처럼 로컬 전용이 아님) — Gateway 커널과 ESP32가 "같은 순서"를
계산하려면 `seed`/`channelCount`/`firstChannel` 등이 한 글자도 안 틀리고
같아야 하기 때문입니다.

### 3.1 왜 기기 한 대씩 순서대로 처리하나

`ota_ack_fields_t`(ACK/NACK 패킷)에는 "어느 기기가 보냈는지" 필드가
없습니다(`session_id`+`acknowledged_type`+`sequence`만 있음). 그래서
여러 기기에 CONFIG를 동시에 뿌리면 어느 ACK이 어느 기기 것인지 구분할
방법이 없습니다 — 그래서 기기 하나씩 "보내고 그 기기 응답만 기다렸다가
다음 기기로" 처리합니다. `discoverDevices()`가 여러 기기 응답을 병렬로
모으는 것과 다른 이유는, DISCOVER_ACK엔 `device_id`가 담겨 있어 구분이
가능하지만 FHSS ACK엔 없기 때문입니다.

### 3.2 2단계 순서 — CONFIG 전부 끝난 뒤에야 ACTIVATE

**"모든 대상의 확인 후 활성화"**: 1단계에서 모든 기기의 CONFIG를 다
끝내고, 성공한 기기 목록이 확정된 뒤에야(2단계) 그 기기들에게 ACTIVATE를
보냅니다. 한 기기라도 아직 CONFIG 확인 전인데 다른 기기부터
ACTIVATE(=SYNCING 진입, 5초 타이머 시작)해버리면 그 기기만 먼저 호핑을
시작해 나머지와 어긋날 수 있기 때문입니다. 기기 하나가 CONFIG에 실패해도
나머지는 계속 진행합니다(부분 실패를 전체 실패로 만들지 않음).

## 4. Gateway 자신의 커널 호핑 — `Cc1101Transport`

```cpp
Cc1101Status configureFhss(const Cc1101FhssConfig &config);
Cc1101Status startFhss(Cc1101FhssRole role);   // Master | Slave
Cc1101Status stopFhss();
Cc1101FhssStatus getFhssStatus();
```

`Cc1101FhssConfig`는 두 부분으로 나뉩니다:

| 구분 | 필드 | 특징 |
|---|---|---|
| RF 프로필(로컬 전용) | `rfBaseFreqHz`/`rfChannelSpacingHz`/`rfSyncWord`/`rfMdmcfg4`/`rfMdmcfg3`/`rfPktctrl1`/`rfPktctrl0` | 팀 공용 고정 상수, 와이어로 안 나감 |
| 호핑 정책 | `seed`/`channelCount`/`firstChannel`/`slotDurationUs` 등 | `FhssHopPolicy`와 값이 같아야 함(3절) |

**상태를 이중으로 안 둡니다** — `Cc1101Transport`는 "지금 호핑 중인지"를
멤버 변수로 따로 기억하지 않고, 필요할 때마다 `getFhssStatus()`로 커널에
직접 물어봅니다. 커널을 단일 진실 공급원(single source of truth)으로
둔 것 — 이중 상태는 어긋날 위험이 있기 때문입니다.

```cpp
struct Cc1101FhssStatus {
    bool     enabled;        // 호핑 켜져 있는지
    bool     synchronized;   // SYNC 획득 완료(SLAVE 기준) — MASTER는 항상 즉시 true
    uint8_t  currentChannel;
    uint32_t generation;
    uint64_t currentSlot;
    uint32_t syncMisses;     // SYNC 패킷 "받는" SLAVE 전용 카운터 — MASTER에선 0
    uint32_t syncPackets;    // 위와 동일
};
```

## 5. 채널 순서 알고리즘 — seed 기반 셔플 (`cc1101_hop.c`)

핵심 요구사항: **같은 `seed`+같은 채널 범위를 쓰면 두 기기가 완전히 같은
순서를 만들어야** 합니다. 매 슬롯마다 무선으로 "다음 채널이 뭐다"를
알려주는 게 아니라, **양쪽이 각자 같은 계산으로 같은 순서표를 미리
만들어두는** 방식입니다 — 그래서 계산 규칙 자체가 한 글자도 안 틀리고
같아야 합니다.

### 5.1 순서표 생성 (`init` — 설정 적용 시 1회)

```c
static u32 cc1101_xorshift32(u32 *state)
{
    u32 x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}
```

xorshift32라는 가벼운 의사난수 생성기(PRNG)를 `seed`로 초기화해서 씁니다
(암호학적으로 안전할 필요는 없고, "결정적이면서도 골고루 섞이면" 충분한
용도).

1. 사용 가능한 채널을 순서대로 채웁니다: `permutation[i] = firstChannel + i`
2. **인덱스 0번(랑데부 채널)은 고정하고 그 이후만 섞습니다** —
   Fisher-Yates 셔플과 비슷하되 뒤에서부터(`count-1`) `i > 2`까지만
   진행해서 0번을 건드리지 않습니다.

```c
for (i = count; i > 2; i--) {
    u32 j = 1 + cc1101_xorshift32(&state) % (i - 1);   // 1 ~ i-1 범위 (0번 제외)
    swap(permutation[i-1], permutation[j]);
}
```

랑데부 채널을 고정하는 이유: SLAVE가 처음 동기화를 찾을 때는 아직 순서를
모르니 **항상 정해진 채널(랑데부 채널, 보통 `firstChannel`)에서만
기다리면 되도록** 하기 위함입니다. 이 채널만큼은 seed와 무관하게 항상
같은 자리(인덱스 0)에 있어야 합니다.

### 5.2 슬롯 → 채널 매핑 (`channel_for_slot` — 매 슬롯마다 호출)

```c
static int cc1101_seeded_channel_for_slot(..., u64 slot, u8 *channel)
{
    u32 index = slot % channel_count;   // (실제로는 do_div() 사용, 5.3절)
    *channel = permutation[index];
    return 0;
}
```

슬롯 번호를 채널 개수로 나눈 나머지가 이번에 방문할 순서표 인덱스입니다.
`slot`이 0, 1, 2, ...로 증가하면 `permutation[]`을 순환하며 채널을 정하는
것뿐이라 계산량이 거의 없습니다 — 매 슬롯 경계마다 부르는 함수라 가벼워야
하기 때문입니다.

### 5.3 32비트 파이 커널에서 나눗셈 주의

```c
slot_tmp = slot;
index = do_div(slot_tmp, count);   // 64비트 % 연산자를 그냥 쓰면 32비트
                                     // 커널 빌드에서 문제가 생길 수 있어
                                     // 커널이 제공하는 do_div() 사용
```

## 6. SYNC 패킷과 동기화 상태 머신 (`cc1101_fhss.c`)

### 6.1 SYNC 패킷 wire format (ESP32와 공유, 13바이트, Little Endian)

```
[type:1][version:1][generation:4][sequence:2][hop_index:1][slot:4]
```

`hop_index`는 `slot % channel_count`가 아니라 **셔플된 순서표에서 선택된
실제 채널의 0-based 인덱스**입니다(`channel - firstChannel`) — ESP32의
`fhss_hop_sequence_get_index()`와 같은 값이 나오도록 맞춘 것입니다.

MASTER(Gateway)는 매 슬롯 채널을 바꾼 뒤 이 SYNC 패킷을 방송합니다.
SLAVE(ESP32)는 이 패킷의 `slot_number`와 수신 시각으로 자신의 슬롯
타이머 기준 시각을 맞춥니다.

### 6.2 상태 머신 (SLAVE 기준 — MASTER는 자기가 기준이라 즉시 `synchronized`)

```
SEARCHING  --(SYNC 1개 수신)-->  ACQUIRING
ACQUIRING  --(연속 3개, CC1101_FHSS_SYNC_ACQUIRE_COUNT)-->  SYNCHRONIZED
SYNCHRONIZED  --(연속 5개 미수신, CC1101_FHSS_SYNC_LOSS_COUNT)-->  SEARCHING
```

- **`ACQUIRING`은 "연속된 실제 슬롯"의 SYNC만 셉니다** — 같은 슬롯을
  중복 세는 걸 방지. 첫 SYNC만으로 동기화 완료로 보지 않는 이유는,
  단발성 오검출(false sync)로 잘못 동기화되는 걸 막기 위함입니다.
- **`SYNCHRONIZED`에서 5번 연속 놓치면 다시 `SEARCHING`으로** 돌아가
  처음부터 다시 찾습니다 — 이 문서 짝 문서(`troubleshooting-fhss-hopping.md`)
  에서 다루는 "SYNC 타임아웃" 증상이 바로 이 재탐색 루프에 갇힌
  상태입니다.

## 7. Qt 화면 연동 현황

"FHSS 활성화" 버튼(`otamanager.cpp::onFhssActivateClicked()`)이 하는 일:

1. `rolloutFhssConfig()`로 3절의 CONFIG→ACTIVATE 배포 (blocking, 최악의
   경우 수 초 — `QThread` 워커로 처리, GUI 스레드는 안 막힘)
2. 성공하면 `configureFhss()`+`startFhss(Master)`로 Gateway 자신도
   4절의 커널 호핑 시작
3. FHSS 핸드셰이크에 쓴 `session_id`를 이후 `OtaSession::start()`에도
   재사용 — "호핑 위에서 파일 전송"이 자연스럽게 이어짐

## 8. 알려진 제약

- **기기 한 대와만 동시에 맺을 수 있습니다** (3.1절, ACK에 송신자 필드가
  없어서 병렬 처리 불가 — 설계 제약, 버그 아님).
- **화면에서 FHSS 위로 파일을 전송할 때 슬롯 충돌 회피가 아직 없습니다**
  — DATA 전송이 슬롯 경계와 겹치면 커널의 SYNC 방송이 밀려 동기화가
  깨질 수 있는데, 이걸 피하는 `SlotAwareTransport`는 지금 CLI에만
  연결돼 있고 화면(`onStartClicked()`)에는 아직 없습니다.
- RF 프로필(`rf*` 필드)은 화면/CLI 입력값이 아니라 코드에 고정 상수로
  박아둔 팀 공용 값입니다 — 바꿀 이유가 없는 값이라 사용자가 손댈 수
  없게 의도적으로 막아뒀습니다.

## 9. 관련 파일

| 역할 | 위치 |
|---|---|
| FHSS_CONFIG/ACTIVATE 배포 | `gateway-ota/OTA_System/session/fhssrollout.h`, `.cpp` |
| Gateway 커널 호핑 제어 | `gateway-ota/OTA_System/transport/cc1101transport.h`, `.cpp` |
| RF/호핑 설정·상태 타입 | `gateway-ota/OTA_System/transport/cc1101_status.h` |
| 화면 연동 | `gateway-ota/OTA_System/ui/otamanager.cpp` (`onFhssActivateClicked()`) |
| SYNC 송수신·상태 머신 | `kernel-cc1101-spi/cc1101_fhss.c` |
| 채널 순서 알고리즘 | `kernel-cc1101-spi/cc1101_hop.c` |
| 실기기 CLI | `gateway-ota/OTA_System/tests/smoke_fhss_activate_main.cpp`, `smoke_fhss_ota_transfer_main.cpp` |
| 증상별 문제 해결 | [`troubleshooting-fhss-hopping.md`](troubleshooting-fhss-hopping.md) |
| 설계 결정 전체 경위 | `docs/note/design-notes-gateway-ota-es.md` 41~48절 |
