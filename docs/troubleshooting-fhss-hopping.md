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
OTA_FHSS_READY -> OTA_RECEIVING      ← 3단계 진입 (OTA_START 수신)
OTA_FHSS_SYNCING -> MENU_OTA         ← 2단계 실패 (SYNC 타임아웃) — 1장 또는 4장
```

| 증상 | 막힌 단계 | 참고 장 |
|---|---|---|
| CONFIG/ACTIVATE 자체가 완전 무응답 (Gateway 쪽 로그에 ACK 한 번도 안 찍힘) | 1단계 진입도 못 함 | 2장 |
| CONFIG/ACTIVATE는 성공, `OTA_FHSS_SYNCING`에서 타임아웃 — **SYNC 패킷이 하나도 안 보임** | 2단계 실패 (싱크워드) | 1장 |
| CONFIG/ACTIVATE는 성공, `OTA_FHSS_SYNCING`에서 타임아웃 — **SYNC는 보이는데 획득을 못 함** | 2단계 실패 (타이밍 교착) | 4장 |
| `OTA_FHSS_READY`까지는 갔는데 DATA의 ACK가 유실됨 | 3단계 실패 | 3장 |
| `OTA_RECEIVING`까지 갔는데 배치의 **절반만** 도착 (누락 seq가 규칙적) | 3단계 실패 (반이중) | 5장 |

> 1장과 4장은 FSM 전이만 보면 똑같습니다. **ESP32 로그에 `SYNC RX:`
> 줄이 찍히는지**로 갈라내세요 — 안 찍히면 1장(싱크워드가 달라 하드웨어
> 필터에서 걸러짐), 찍히는데 `state=SEARCHING`에서 못 벗어나면 4장입니다.

---

## 1. 핸드셰이크는 성공, SYNC 패킷이 아예 안 보임 — 싱크워드 불일치

> **먼저 4장과 구분하세요.** 이 장은 ESP32 로그에 `SYNC RX:` 줄이
> **한 번도 안 찍히는** 경우입니다. 찍히는데 획득만 못 하면 4장입니다.

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
... (타임아웃까지 경과 — `SYNC RX:` 줄이 단 한 번도 안 찍힘)
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

## 3. SYNC는 잡히는데 OTA_DATA의 ACK가 계속 유실됨 — ESP32 ACK가 다음 홉 채널에서 나감

### 증상

호핑 동기화(`synchronized=1`)까지는 성공하고 START 핸드셰이크도 ACK를
받는데, 그다음 배치 DATA 전송에서 특정 seq만(또는 batchSize=1이어도
전부) 5회 재시도 후 실패합니다.

```
[fhss_ota][log] seq=1 재전송 (사유=timeout, 재시도 1/5)
...
[fhss_ota][log] FAIL: 배치 재전송 한도(5회) 초과 (sequence=1)
```

**Gateway 쪽에서 슬롯당 패킷 수를 줄여도(batchSize=1) 재현됩니다** —
패킷이 한 슬롯에 몰려서가 아니라는 뜻입니다.

### 원인 — ESP32 내부 처리 지연이 자신의 홉 경계를 넘김

ESP32 로그를 정밀 대조하면 원인이 정확히 보입니다.

```
43131  RX path=FHSS ch=8 type=DATA(2)          DATA(seq=0)가 채널 8에서 도착
43172  RX data drain stopped: result=3 ch=8    드레인 루프가 타임아웃으로 포기
43179  channel selected: slot=10 channel=4     홉 스케줄러가 이미 채널 4로 전환
43186  DATA accepted: seq=0                     그제서야 DATA 처리 시작
43197  TX ACK: seq=0                            ACK 생성
43203  TX_QUEUE path=FHSS ch=4 type=ACK         이미 채널 4로 넘어간 뒤라 ACK가 ch=4에서 나감
43224  TX_RESULT ch=4 status=0 (성공)           무선 송신 자체는 정상 성공
```

**무선 송신은 실패하지 않습니다.** ESP32는 ACK를 물리적으로 잘
쐈습니다(`status=0`) — 다만 DATA를 받은 채널(8)이 아니라 그 사이 넘어간
다음 채널(4)에서 쐈을 뿐입니다. Gateway는 자기 홉 스케줄에 따라 그
순간 다른 채널을 듣고 있으니, ACK가 물리적으로는 전파를 탔지만
Gateway 귀에는 안 들리는 것입니다.

지연의 근원은 `fhss_service.c`의 `drain_rx_data_until()`입니다. 이
함수는 슬롯 안에서 DATA를 계속 받으려고 시도하다가, 한 번이라도
`RECEIVE_RESULT_RADIO_ERROR`(동기워드는 잡았는데 본문 읽기가
타임아웃 — 2026-08-17 주석에 이미 기록된 알려진 현상)를 만나면 그
즉시 `return false`로 드레인을 완전히 중단합니다. 이 처리 + 그 위의
`ota_consumer`가 DATA를 실제로 소비하고 ACK를 만드는 데 걸리는 시간이
합쳐지면, 슬롯 경계(`switch_time_us`)를 넘기기 충분합니다 — 그리고
`RECEIVE_RESULT_RADIO_ERROR`는 진짜 하드웨어 오류와 "그냥 타이밍만
빠듯했던 경우"를 구분하지 못하는 하나의 값이라, 후자로 인한 지연도
그대로 누적됩니다.

### Gateway 쪽에서는 못 고침

`batchSize`를 줄이거나 패킷 전송 속도를 늦추는 시도는 **효과가
없습니다** — Gateway가 몇 개를 얼마나 촘촘히 보내는지와 무관하게,
ESP32 혼자서 자신의 홉 타이머보다 처리가 늦어지는 문제이기 때문입니다.
2026-08-24 실기기 테스트에서 batchSize=1(패킷 하나만 보내고 ACK
기다림)로도 동일하게 재현됨을 확인했습니다.

### 해결 (2026-08-24, `firmware-esp32` `dda9572`에서 수정 완료)

`RECEIVE_RESULT_RADIO_ERROR`를 두 가지로 분리했습니다.

- `rf_transport_start_receive()` 실패 — 진짜 하드웨어 오류. 기존대로
  `report_event(FHSS_SERVICE_EVENT_ERROR)`.
- `rf_transport_receive_packet()` 본문 타임아웃 — 새 값
  `RECEIVE_RESULT_BODY_TIMEOUT`. TIMEOUT/CRC_FAIL과 동일하게 취급해
  드레인을 계속하고, tracking 중이면 `handle_miss()`만 호출.

수정 후 실기기 로그에서 `drain stopped` / `RADIO_ERROR` 문자열이 **한
번도 안 나타나는 것**으로 이 경로가 막힌 걸 확인했습니다. 다만 이걸
고쳐도 전송은 여전히 실패했는데, 원인이 4장/5장으로 옮겨간 것이었습니다.

### 관련 로그 전문

Gateway 쪽 `perf/fhss-slot-batch-tx` 브랜치, `ota_smoke_fhss_ota_transfer`
CLI로 재현. 실기기: Pi 149 + ESP32(A2-9E-60). 2026-08-24.

---

## 4. `OTA_FHSS_SYNCING`에서 타임아웃 — 동기화 전에 DATA를 쏴서 동기화가 막힘

### 증상

3장과 달리 **아예 `OTA_FHSS_READY`로 못 넘어가고** `OTA_FHSS_SYNCING`에서
타임아웃으로 `MENU_OTA`로 되돌아갑니다. 1장(싱크워드 불일치)과 증상이
같아 보이지만, **SYNC 패킷 자체는 정상적으로 수신되고 있다**는 점이
다릅니다.

```
36671  fsm: OTA_FHSS_CONFIGURED -> OTA_FHSS_SYNCING   (slot=0, channel=1 고정)
36719~38723  channel selected: slot=0 channel=1 x6    2.4초간 SYNC 전무
39083  SYNC RX: state=SEARCHING slot=8 channel=1       첫 SYNC (2.4초 걸림)
39109  RX START(1)                                     26ms 뒤 Gateway가 벌써 START
39319  RX DATA(2) ...                                  DATA 폭주 시작
39358  channel selected: slot=9 channel=7              호핑 추종 시작
40041  channel selected: slot=0 channel=1              ★ 랑데부로 리셋 = SEARCHING 복귀
41483  SYNC RX: state=SEARCHING slot=16                 처음부터 다시
41711  fsm: OTA_FHSS_SYNCING -> MENU_OTA                ★ 타임아웃, 세션 폐기
41783  SYNC RX: state=SYNCHRONIZING                     DATA 멈추자마자
42083  SYNC RX: state=SYNCHRONIZING
42095  SYNC_ACQUIRED                                    400ms만 더 버텼으면 성공
```

**결정적 단서**: 세션이 폐기되어 DATA가 멈추자마자 300ms 만에 깨끗이
동기화가 됩니다. "DATA 폭주가 동기화를 방해하고 있었다"는 증거입니다.

### 원인 — 두 코드의 타이밍 가정이 안 맞음

1. ESP32는 ACTIVATE 직후 **랑데부 채널(보통 1번)에 고정**된 채 SYNC를
   기다립니다. 그런데 Gateway는 이미 8채널을 순회 중이라 1번으로 돌아오는
   건 **8슬롯마다 = 300ms × 8 = 2.4초에 한 번**뿐입니다.
2. 획득하려면 SYNC를 **연속 3개** 받아야 합니다
   (SEARCHING → SYNCHRONIZING ×2 → 획득) → +0.9초. **최악 합계 3.3초.**
3. 그런데 Gateway는 `kSyncSettleMs = 2000` — **2초만 기다리고** START/DATA를
   쐈습니다. 위 로그에선 ESP32의 첫 SYNC(2.4초)보다도 먼저 도착했습니다.
4. **치명타**: DATA를 처리하느라 다음 SYNC를 놓치면, ESP32는 아직 획득
   전(SYNCHRONIZING)이라 관용 없이 기준점을 통째로 버리고 랑데부로
   되돌아갑니다(`fhss_service.c` `handle_miss()`의 `was_synchronizing`
   분기 → `fhss_slot_scheduler_clear_reference()`). → 또 2.4초 대기 →
   그 사이 DATA는 계속 옴 → 반복 → 영영 TRACKING 못 감.
5. ESP32의 동기화 타임아웃이 먼저 터져 세션 폐기.

요약하면 **"ESP32가 동기화하기 전에 Gateway가 데이터를 퍼붓고, 그 데이터
때문에 ESP32가 동기화를 못 하는"** 구조적 교착입니다.

### 해결 (2026-08-24, 양쪽 동시 수정)

두 값은 **Gateway 대기 < ESP32 타임아웃** 관계로 맞물려 있어서 한쪽만
바꾸면 안 됩니다.

| 파일 | 상수 | 변경 | 이유 |
|---|---|---|---|
| `gateway-ota` `smoke_fhss_ota_transfer_main.cpp` | `kSyncSettleMs` | 2000 → **4000** | 최악 3.3초 + 여유. 근본 원인(동기화 전 DATA 송신)을 직접 막음 |
| `firmware-esp32` `main/fsm.c` | `OTA_FHSS_SYNC_TIMEOUT_MS` | 5000 → **10000** | 5초는 정상 경로에도 여유가 1.7초뿐. 지터 흡수용 안전망 |

기존 주석의 "SYNC_ACQUIRED까지 1~2초"는 **첫 SYNC를 이미 잡은 뒤부터**
재는 시간이었고, 그 앞의 "첫 SYNC를 잡기까지"(최악 2.4초)를 빠뜨린 것이
오류였습니다.

**검증**: 수정 후 FSM이 처음으로
`OTA_FHSS_SYNCING → OTA_FHSS_READY → OTA_RECEIVING`까지 정상 진행하고
`OTA progress: 0%`까지 찍혔습니다.

---

## 5. 동기화·핸드셰이크 다 성공했는데 배치의 절반만 도착 — 반이중 왕복 시간 무시

### 증상

4장까지 다 통과해서 `OTA_RECEIVING`까지 갔는데도 배치 재전송 한도를
넘겨 실패합니다. 특징은 **누락되는 seq가 규칙적**이라는 점입니다.

```
Gateway: window_elapsed_ms=0, 16, 32, 48, 65    (한 슬롯에 5개를 16ms 간격으로)

ESP32:
  23185  RX DATA(seq=0)
  23234  DATA accepted seq=0
  23248  TX ACK seq=0
  23306  TX_RESULT ch=1                      ACK 송신 완료 → 왕복 121ms
  23337  batch store seq=2, received=0x05    ★ seq=1은 아예 못 받음
  23403  TX_RESULT (seq=2 ACK)
  23449  batch store seq=4, received=0x15    ★ seq=3도 못 받음
```

`missing_mask`가 `0x1A`(seq 1,3,4)에 고정된 채 재전송만 반복합니다.

### 원인 — CC1101은 반이중, ACK 송신 중엔 귀가 닫힘

CC1101은 **반이중(half-duplex — 송신과 수신을 동시에 못 하는 방식)**
트랜시버 하나뿐입니다. ESP32가 ACK를 송신하는 동안은 수신을 못 합니다.

- ESP32의 DATA → ACK 왕복 실측: **121ms**
- Gateway가 보낸 간격: **16ms**

그래서 seq=0의 ACK를 보내는 사이 seq=1이 지나가고, seq=2의 ACK를 보내는
사이 seq=3이 지나갑니다. **패킷을 촘촘히 보낼수록 오히려 덜 도착하는**
상태였습니다.

"슬롯당 여러 패킷" 최적화가 슬롯 경계만 신경 쓰고 **수신 측의 처리 왕복
시간**이라는 제약을 아예 고려하지 않은 것이 원인입니다.

### 해결 (2026-08-24, Gateway 쪽만 수정)

`SlotAwareTransport::ensureSafeWindow()` 맨 앞에서, 직전 전송 이후
`kMinPacketGapMs = 150`(실측 121ms + 여유)이 지날 때까지 대기하도록
했습니다. 창 재사용 경로와 새 창 경로 양쪽에 다 적용됩니다.

### 슬롯당 몇 개가 나가는가

| 항목 | 값 |
|---|---|
| 슬롯 길이 | 300ms |
| 앞 가드 `kPostSyncGuardMs` | 25ms |
| 뒤 가드 `kTailGuardMs` | 40ms |
| **사용 가능한 안전창** | **235ms** (300 − 25 − 40) |
| 최소 간격 `kMinPacketGapMs` | 150ms |

전송 가능한 시점은 `0ms`, `150ms` 두 번. 세 번째는 `300ms`가 되어
안전창도 슬롯도 넘으므로 다음 슬롯으로 밀립니다.

→ **슬롯당 2개.** 가드를 아무리 줄여도 `300 ÷ 150 = 2`라서 **구조적
상한**입니다. 처리량은 초당 약 6.7개, 7949청크 기준 약 20분(이론값).

### 더 빠르게 하려면 — 배치 단위 ACK (다음 과제)

상한 2개를 만드는 건 **"DATA 하나당 ACK 하나"**라는 응답 방식 자체입니다.
ESP32가 **배치 종료 시점(또는 배치 타임아웃)에 한 번만** 응답하도록
바꾸면 버스트 내내 수신 상태를 유지할 수 있어 간격 제약이 사라집니다.

구조적으로는 이미 가능합니다.

- 프로토콜에 `missing_mask` 기반 배치 개념이 이미 있음
  (`batch store: base=0, seq=2, received=0x05, missing=0x1A`)
- ESP32에 배치 타임아웃 시 누락분만 골라 NACK를 보내는 경로가 이미 있음
  (`RX timeout: ... missing_mask=0x1A` → `TX NACK ... seq=1/3/4`)

즉 "DATA마다 즉시 보내는 개별 ACK"만 없애고 배치 종료 시점의 집계
응답에 맡기면 됩니다. Gateway `OtaSession`도 개별 ACK 대신 배치 응답
하나를 기다리도록 맞춰야 합니다.

**단, 전송 완주가 확인되기 전에는 손대지 마세요** — 실패했을 때 원인이
어느 쪽인지 다시 갈라내야 합니다.

---

## 6. 같은 증상, 다른 원인 — 오늘 겪은 네 겹

`FAIL: 배치 재전송 한도(5회) 초과`라는 **동일한 증상** 하나 뒤에 서로
다른 계층의 원인이 네 개 겹쳐 있었습니다. 하나를 벗겨야 다음 게
드러나는 구조였으므로, 비슷한 증상을 만나면 이 순서로 의심하세요.

| # | 계층 | 원인 | 장 |
|---|---|---|---|
| 1 | 커널 드라이버 | 모듈이 레지스터를 안 써서 무선 통신 자체가 안 됨 | `kernel-cc1101-spi` 쪽 문서 |
| 2 | ESP32 슬롯 로직 | 본문 타임아웃을 무선 오류로 오분류 → ACK가 다음 홉 채널로 | 3장 |
| 3 | 동기화 타이밍 | 동기화 전에 DATA를 쏘고, 그 DATA가 동기화를 막는 교착 | 4장 |
| 4 | 반이중 제약 | 수신 측 ACK 왕복(121ms) 무시하고 16ms 간격 전송 | 5장 |

**교훈**: 증상 문자열이 같다고 원인이 같은 게 아닙니다. 매번 **ESP32
시리얼 로그를 밀리초 단위로 Gateway 로그와 대조**해서 "정확히 어느 순간
무엇이 어긋났는지"를 짚은 것이 네 번 다 결정적이었습니다. Gateway 로그만
봤다면 네 번 모두 "ACK를 못 받았다"까지밖에 못 갔을 것입니다.

### ESP32 로그 뽑는 법

로그가 매우 길어(수백 KB) 통째로 복사하면 앞부분만 남고 잘립니다.
파일로 저장한 뒤 필요한 줄만 검색하세요.

```powershell
idf.py -p COM9 monitor | Tee-Object -FilePath esp_log.txt
# 테스트 실행 → 끝나면 Ctrl+] 로 모니터 종료
# 새 창에서:
Select-String -Path esp_log.txt -Pattern "drain stopped|BODY_TIMEOUT|RADIO_ERROR|SYNC RX|fsm: |batch store"
```

`App version:` 줄도 꼭 확인하세요 — 빌드/플래시가 실제로 반영됐는지
가장 빠르게 판별하는 방법입니다. (여러 대의 PC에 저장소 복제본이 따로
있으면, 커밋한 쪽과 플래시한 쪽이 달라 수정이 반영 안 된 채 테스트하는
일이 실제로 있었습니다.)

---

## 7. 참고

| 문서 | 용도 |
|---|---|
| `docs/note/design-notes-gateway-ota-es.md` 41~42절 | 1장(싱크워드) 원인 규명 전체 경위 |
| `docs/note/design-notes-gateway-ota-es.md` 48절 | 2장(MENU_OTA) 진단 경위 |
| `docs/note/design-notes-gateway-ota-es.md` 50절 | 3장(ACK 채널 유실) 진단 경위 |
| `docs/note/design-notes-gateway-ota-es.md` 54절 | 4장(동기화 교착) 진단 경위 |
| `docs/note/design-notes-gateway-ota-es.md` 55절 | 5장(반이중 간격) 진단 경위 + 처리량 계산 |
| `kernel-cc1101-spi/docs/troubleshooting-cc1101.md` | 그 아래(RF 물리 계층) 문제 — 인터럽트가 아예 안 울리는 경우 등 |
| `firmware-esp32/fhss-ota-radio/main/fsm.c` | FSM 전이표·메뉴 게이팅·동기화 타임아웃 상수 |
| `firmware-esp32/fhss-ota-radio/components/fhss_service/fhss_service.c` | 3장의 `drain_rx_data_until()`, 4장의 `handle_miss()` |
| `OTA_System/tests/smoke_fhss_ota_transfer_main.cpp` | 4·5장의 `kSyncSettleMs` / `kMinPacketGapMs` / `SlotAwareTransport` |
