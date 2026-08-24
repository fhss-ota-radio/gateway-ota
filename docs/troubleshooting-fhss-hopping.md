# FHSS 호핑 트러블슈팅 가이드

> 2026-08-22~24 실기기 디버깅에서 겪은 "호핑(FHSS)이 안 됨" 증상을
> 정리했습니다. `kernel-cc1101-spi/docs/troubleshooting-cc1101.md`(RF
> 물리 계층 전반)와 짝을 이루는 문서로, 이쪽은 **FHSS 핸드셰이크/동기화
> 단계**에 집중합니다.

---

## 0. 시작하기 전에 — "핸드셰이크 성공"과 "호핑 성공"은 다른 계층입니다

FHSS 흐름은 두 단계로 나뉩니다.

```
1단계: FHSS_CONFIG → FHSS_ACTIVATE     (채널 0, OTA 전용 싱크워드로 오가는 대화)
2단계: SYNC 패킷 교환 → 호핑 동기화     (호핑 채널들, 팀 공용 싱크워드로 오가는 대화)
```

**1단계가 성공(ACK 수신)했다고 2단계도 성공한다는 보장이 없습니다** —
서로 다른 채널·다른 싱크워드를 쓰기 때문입니다. ESP32 로그의 FSM 전이로
지금 어느 단계에서 막혔는지 먼저 확인하세요.

```
MENU_OTA -> OTA_FHSS_CONFIGURED   ← 1단계 성공 (CONFIG ACK)
OTA_FHSS_CONFIGURED -> OTA_FHSS_SYNCING   ← 1단계 완전 성공 (ACTIVATE ACK)
OTA_FHSS_SYNCING -> OTA_FHSS_READY   ← 2단계 성공 (SYNC 획득)
OTA_FHSS_SYNCING -> MENU_OTA (5초 후)   ← 2단계 실패 (SYNC 타임아웃) — 1장
```

| 증상 | 막힌 단계 | 참고 장 |
|---|---|---|
| CONFIG/ACTIVATE 자체가 완전 무응답 (Gateway 쪽 로그에 ACK 한 번도 안 찍힘) | 1단계 진입도 못 함 | 2장 |
| CONFIG/ACTIVATE는 성공, `OTA_FHSS_SYNCING`에서 5초 뒤 `MENU_OTA`로 복귀 | 2단계 실패 | 1장 |

---

## 1. 핸드셰이크는 성공, SYNC가 5초 안에 안 잡힘 — 싱크워드 불일치

### 증상

```
[fhss_activate][log]   수신: kind=Ack ... acknowledged_type=8 (매칭됨)  ← CONFIG 성공
[fhss_activate][log]   수신: kind=Ack ... acknowledged_type=9 (매칭됨)  ← ACTIVATE 성공
  - device_id=0x... -> Activated
```
```
fsm: MENU_OTA -> OTA_FHSS_CONFIGURED
fsm: OTA_FHSS_CONFIGURED -> OTA_FHSS_SYNCING
fhss_service: channel selected: slot=0 channel=1   (SYNC 기다리는 중, 계속 channel=1 고정)
... (5초 경과)
fsm: OTA_FHSS_SYNCING -> MENU_OTA      ← SYNC 타임아웃, 되돌아감
```

**프로토콜 핸드셰이크는 완벽히 성공했는데, 그 다음 무선 동기화만 실패**하는
패턴입니다.

### 원인 — RF 프로필의 싱크워드를 헷갈림

CC1101은 **싱크워드(SYNC1/SYNC0)가 정확히 일치해야만** "패킷이 왔다"고
인식하는 하드웨어 레벨 필터입니다. 다르면 인터럽트도 안 울리고 FIFO에도
안 들어옵니다 — 두 기기가 같은 채널·같은 주파수에 있어도 서로의 신호를
아예 "패킷"으로 인식하지 못합니다.

이 프로젝트는 싱크워드를 **용도별로 2개** 씁니다:

| 용도 | SYNC1/SYNC0 | 왜 이 값을 CONFIG/ACTIVATE에서는 못 걸렸나 |
|---|---|---|
| OTA — 채널 0 고정 | `0x2D` / `0xD4` | CONFIG/ACTIVATE는 채널 0에서만 오간 대화라 이 값으로 정상 통신됨 |
| FHSS 호핑 채널 (1번 이상) | `0xD3` / `0x91` (팀 공용) | **SYNC 패킷은 호핑 채널에서 오감 — 여기서 값이 갈렸음** |

원래 `smoke_fhss_activate_main.cpp`의 기본 RF 프로필이 `0x2DD4`(OTA 전용
값)를 그대로 썼는데, `firmware-esp32`의 `components/rf_transport/rf_transport.c`
(`s_433mhz_settings[]`)를 직접 열어보니 FHSS 호핑 채널에서는 원래
팀 공용이었던 `0xD391`을 쓰고 있었습니다. CONFIG/ACTIVATE는 채널 0에서만
오가서 이 불일치에 안 걸렸고, 그 다음 SYNC만 호핑 채널(다른 싱크워드)에서
오가려다 보니 거기서 처음 막힌 것입니다.

### 확인 방법

1. Gateway 쪽 RF 프로필 상수와 `firmware-esp32`의 실제 레지스터 설정값을
   나란히 대조합니다 (`sync_word`, `base_freq_hz`, `channel_spacing_hz`,
   `mdmcfg3/4`, `pktctrl0/1`).
2. **이 값들은 `ota_protocol.h`의 와이어 프로토콜에 안 실리는 "로컬 전용"
   값이라 프로토콜 레벨 검증만으로는 못 잡습니다** — CONFIG/ACTIVATE가
   성공해도 안심할 수 없는 이유입니다.

### 해결

```diff
- kernelConfig.rfSyncWord = 0x2DD4u;   // OTA 전용 값 (채널 0에서만 유효)
+ kernelConfig.rfSyncWord = 0xD391u;   // FHSS 호핑 채널의 팀 공용 값
```

`rfBaseFreqHz`/`rfChannelSpacingHz`도 ESP32 레지스터값에서 역산한 정확한
값(`433919830` / `199951`)으로 같이 갱신했습니다(기존 추정치와 오차는
무시 가능한 수준이었음).

커밋: `1320a02 fix(fhss): RF 프로필 싱크워드 정정(0x2DD4 -> 0xD391)`
(2026-08-22). 상세 로그·경위: `docs/note/design-notes-gateway-ota-es.md` 42절.

### 재발 방지

- **RF 프로필 값(주파수/싱크워드/채널 간격)은 팀 공용 상수이지 화면·CLI
  입력값이 아닙니다** — 사용자가 바꿀 이유가 없고, 바뀌면 이런 사고가
  재현됩니다. 코드에 고정 상수로 박아두고, 바꿀 일이 생기면 `firmware-esp32`
  쪽과 반드시 먼저 맞춰야 합니다.
- 이 값들에 확신이 없다면 코드 주석에 "추정치, 재확인 필요"라고 명시해두는
  습관이 실제로 디버깅 시간을 줄여줬습니다(추정치인 걸 몰랐다면 이 원인을
  찾는 데 더 오래 걸렸을 것).

---

## 2. CONFIG/ACTIVATE 자체가 완전 무응답 — ESP32가 `MENU_OTA`에 없음

### 증상

```
[fhss_ota][rollout] CONFIG 시도 1/6 — transport.send()=성공
[fhss_ota][rollout]   300ms 동안 아무 패킷도 안 들어옴(완전 무응답)
... (6회 전부 동일)
[fhss_ota] FHSS 활성화 실패 — 파일 전송을 시작하지 않습니다.
```

**1장과 달리 CONFIG 단계부터 완전 무응답**입니다 — 1단계(채널 0, OTA 전용
싱크워드)조차 성공을 못 합니다. 싱크워드/주파수 문제라면 이 단계는
원래 통과되므로(1장 표 참고), 원인이 다릅니다.

### 확인 — FHSS와 무관한 `ota_smoke_discover`로도 재현되는지

```sh
./ota_smoke_discover /dev/cc1101 2000
```
이것도 무응답이면 FHSS 코드 문제가 아니라 **ESP32가 OTA 관련 패킷을
아예 처리 안 하는 상태**라는 뜻입니다 — `DISCOVER`도 `MENU_OTA`
상태에서만 응답하도록 설계돼 있기 때문입니다.

### 원인 — ESP32의 메뉴 상태(FSM)가 `MENU_OTA`가 아님

ESP32 시리얼 로그(`idf.py monitor`)를 보면 부팅 직후엔 항상
`MENU_COMM`(통신/일반 모드)입니다:

```
I (448) fsm: BOOT_INIT -> MENU_COMM
```

OTA용 CONFIG/DISCOVER 패킷 처리기는 `fsm_ota_mode_callback()`이
`FSM_STATE_MENU_OTA`(및 그 하위 상태)일 때만 true를 반환하도록 게이팅돼
있습니다(`firmware-esp32/fhss-ota-radio/main/fsm.c`). `MENU_COMM`에
머물러 있으면 CONFIG를 받고도 그냥 무시합니다.

### 해결 — 로터리 인코더로 OTA 메뉴 진입

로터리 인코더를 **돌리는 것만으로는** 전이가 안 일어납니다 — 회전은
화면에 미리보기(흰 테두리)만 갱신하고, **눌러서(클릭) 확정**해야
`MENU_SELECT_OTA` 이벤트가 발생해 `MENU_COMM → MENU_OTA`로 실제 전이됩니다
(`fsm.c`의 `on_menu_select()`). 확정되면 로그에 아래가 찍히고 화면에
"STANDBY"가 흐르는 문구로 표시됩니다:

```
fsm: MENU_COMM -> MENU_OTA
```

### 부수 확인 — generation은 이전 값보다 커야 함

ESP32는 마지막으로 성공한 FHSS `generation`을 NVS(플래시)에 저장해뒀다가
부팅 시 복원합니다:

```
fhss_audio_adapter: ready: RX standby, ... generation=8 source=nvs-active
```

`MENU_OTA` 진입 문제와는 별개로, 다음 CONFIG 시도의 `generation` 값이
이 저장된 값보다 **작거나 같으면** 낡은 설정으로 거부될 수 있습니다.
재시도할 때는 이전에 성공했던 값보다 큰 숫자를 쓰세요(예: 유닉스
타임스탬프 기반 값 — 실제 날짜가 안 맞아도 계속 커지기만 하면 됩니다).

---

## 3. 참고

| 문서 | 용도 |
|---|---|
| `docs/note/design-notes-gateway-ota-es.md` 41~42절 | 1장(싱크워드) 원인 규명 전체 경위 |
| `docs/note/design-notes-gateway-ota-es.md` 48절 | 2장(MENU_OTA) 진단 경위 |
| `kernel-cc1101-spi/docs/troubleshooting-cc1101.md` | 그 아래(RF 물리 계층) 문제 — 인터럽트가 아예 안 울리는 경우 등 |
| `firmware-esp32/fhss-ota-radio/main/fsm.c` | FSM 전이표·메뉴 게이팅 로직 원본 |
