# gateway-ota 마일스톤 · 일정표

`gateway-ota`(라즈베리파이 OTA 매니저 Qt 앱) 전체 진행 상황과 앞으로의 계획을
한 문서로 정리한 것. 완료된 것은 기록으로 남기고, 남은 것은 실행 가능한 태스크
단위로 쪼갰다.

> 기준일: **2026-08-17** (정식 경로 실기기 전송 성공, 파일 무결성 검증 대기)
> 담당: 팀원3, 4 — 그중 OTA 세션 로직(`core/`·`session/`)·화면(`ui/`)은 이 문서
> 작성자가 담당, CC1101 적용 세부(유니캐스트 주소, FHSS 채널 호핑 등)는 팀원이
> 별도 담당

---

## 전체 마일스톤 요약

| 마일스톤 | 내용 | 상태 |
|---|---|---|
| 1 | Qt 프로젝트 세팅 및 화면 뼈대 | ✅ 완료 |
| 2 | 전송 계층 추상화 (`ITransport`) + `Cc1101Transport` 실구현 | ✅ **완료 (2026-08-16 실기기 검증 통과, 1067/1067)** |
| 3 | BIN 분할 + CRC (`ota-protocol` 연동) | ✅ 완료 |
| 4 | 핸드셰이크 + 단순 송수신 + ACK (당초 계획이던 `OtaSession` FSM 통짜 구현 대신, 더 작은 단위로 쪼개서 점진적으로 구현) | 🟡 진행 중 — 핸드셰이크·개별 ACK까지 완료, **배치 ACK·재전송·`OtaSession` 통합은 미착수 → 남은 최대 덩어리** |
| 5 | 실기기 통합 검증 (라즈베리파이 2대) | 🟡 거의 완료 — 정식 경로 파일 전송 성공, **`sha256sum` 무결성 검증만 남음** |

> **[2026-08-17 갱신 안내]** 이 문서의 이전 버전(8/15 기준)은 마일스톤 2를
> "커널 드라이버 문제로 검증 보류"로, 마일스톤 5를 "디버깅 중"으로 적어뒀습니다.
> 그 사이 커널 드라이버 문제가 전부 해소돼서 **정식 경로로 파일 전송이
> 성공했습니다.** 4절이 그 결과로 다시 쓰였습니다.

> **마일스톤 4가 원래 계획과 달라진 이유**: 애초엔 `OtaSession`이라는 FSM
> 클래스를 한 번에 설계해서 구현하려 했는데(Day1~5 세부 일정, 이 문서 예전
> 버전 참고), 실제로는 "일단 ACK 없이 단순히 보내보기" → "받는 쪽 스텁" →
> "받는 쪽 ACK 응답" → "보내는 쪽이 그 ACK를 기다리는 진짜 핸드셰이크"
> 순서로 작은 단위씩 실기기로 검증하며 쌓아올리는 방식으로 진행함. 이유는
> 한 번에 큰 걸 만들면 실기기 검증 없이 가정으로 짠 코드가 쌓이는 위험이
> 커서 — `docs/note/design-notes-gateway-ota-es.md` 14~16절에 각 단계별
> 결정 기록 있음.

---

## 1. 지금 파일 구성 (`OTA_System/`)

폴더별 역할 원칙: `core/` = 순수 데이터 변환(파일→패킷 조각), I/O 없음.
`transport/` = I/O 담당(실제 무선 송수신). `session/` = 그 둘을 엮어서
"전송 흐름"을 만드는 조율 계층. `ui/` = Qt 화면. `tests/` = 실행 가능한
검증용 프로그램(자동 테스트 + 실기기 수동 테스트).

| 파일 | 역할 | 상태 |
|---|---|---|
| `ui/otamanager.h/.cpp/.ui` | Qt 메인 화면 (연결/전송대상/BIN파일/진행률/로그 카드) | 화면 뼈대만 완료, 실제 전송 로직과는 아직 연결 안 됨 |
| `core/binsplitter.h/.cpp` | `.bin` 파일을 `ota_protocol.h` 규격 청크(최대 48byte)로 분할, CRC16 포장 | 완료, 유닛테스트 통과 |
| `transport/itransport.h` | 전송 계층 추상 인터페이스 (open/close/isOpen/send/recv) — 구현체를 갈아끼울 수 있게 하는 설계 | 완료 |
| `transport/cc1101transport.h/.cpp` | **정식** `ITransport` 구현체. `/dev/cc1101`(커널 드라이버가 만드는 파일)을 열어서 write/read/ioctl로 제어 | **완료 — 실기기 검증 통과(1067/1067, 2026-08-16)** |
| `transport/cc1101_ioctl.h`, `cc1101_status.h` | 커널 드라이버와 공유하는 계약 헤더(ioctl 번호·구조체), 상태값 타입 | 완료 |
| `session/simplesender.h/.cpp` | `simpleSendFile()`(ACK 없이 순서대로 쏘기만 함) + `performHandshake()`(START 보내고 ACK 올 때까지 재시도) + `sendDataAndEnd()` | 완료 |
| `session/simplereceiver.h/.cpp` | `tryReceiveOnce()`(패킷 하나 받아서 종류 구분) + `sendAckFor()`(받으면 반사적으로 ACK 응답, 재전송 판단은 안 함) | 완료 |
| `tests/tst_binsplitter.cpp` | `BinSplitter` 자동 유닛테스트 (ctest 등록됨) | 완료 |
| `tests/smoke_send_main.cpp`, `smoke_recv_main.cpp` | `Cc1101Transport` 기반 실기기 수동 테스트 CLI (ctest 미등록) | **실기기 검증 완료.** `smoke_recv`는 2번째 인자로 받은 파일 재조립까지 지원(무결성 검증은 미실행) |
| `tools/spidev/` (폴더 전체) | **[진단 도구, 제품 코드 아님]** 커널 계층을 우회해 `/dev/spidevX.Y`로 CC1101을 직접 폴링 — "원인이 하드웨어냐 커널이냐"를 가르는 통제 실험용. `ota_core`에 안 들어감 | 유지 (삭제 조건은 `tools/spidev/README.md`) |

> `session/simplesender.h/.cpp`에 핸드셰이크 로직을 처음엔 별도
> `session/handshake.h/.cpp`로 분리했다가, "파일 개수를 늘리고 싶지
> 않다"는 판단으로 다시 합침(2026-08-14) — 앞으로도 새 기능을 무조건
> 새 파일로 만들지 않고, 계약이 안 부딪히면 기존 파일에 합치는 걸
> 기본으로 함.

---

## 2. 완료된 것 (마일스톤 1~3)

### 마일스톤 1 — Qt 프로젝트 세팅 및 화면 뼈대 ✅
- Qt 프로젝트 생성 (Widgets, CMake) — `OTA_System/`
- 화면 5개 카드 구현 — 연결(Transport) · 전송 대상 · BIN 파일 · 진행률 · 로그
- 위젯 배치를 `otamanager.ui`(Qt Designer XML)로 분리
- `QSettings` 기반 마지막 설정값 저장/복원

### 마일스톤 2 — 전송 계층 추상화 ✅ (실기기 검증까지 완료)
- `ITransport` 인터페이스 설계 (open/close/isOpen/send/recv)
- `Cc1101Status`/`Cc1101RxMetadata` 타입 정의
- **`core/`·`transport/` Qt 의존성 완전히 제거** — `g++`만으로 컴파일·테스트 가능
- **`Cc1101Transport` 실제 구현 완료** — `open()`(POSIX `O_RDWR|O_NONBLOCK`),
  `send()`(`write()`), `recv()`(`poll()`+`read()`+`ioctl(GET_STATUS)`),
  `setChannel`/`startRx`/`flushRx`/`flushTx`(각각 `ioctl()`)
- **실기기 검증 완료 (2026-08-16)** — 51200byte / 1067청크 전량 수신,
  디코딩 실패 0건, seq 0~1066 연속. 4절 참고

### 마일스톤 3 — BIN 분할 + CRC ✅
- `ota-protocol` 공유 레포 연동 (자체 CRC32 버전 삭제, 공유 `ota_protocol.h` 사용)
- `BinSplitter` 클래스 — 48byte 단위 분할, 헤더+CRC16 포장, 패딩 없음
- `ota_core` 정적 라이브러리 분리, 유닛테스트 7개 케이스 통과 (`g++`로 CMake 없이도 실행 가능)

---

## 3. 진행 중 — 마일스톤 4 (핸드셰이크 + 단순 송수신 + ACK)

- [x] `simpleSendFile()` — OTA_START→DATA 전부→OTA_END를 ACK 대기 없이 순서대로 전송
- [x] `tryReceiveOnce()` — 받은 패킷 하나를 종류별로 디코딩
- [x] `sendAckFor()` — Start/Data/End 받으면 반사적으로 ACK 응답 (재전송 판단 없음)
- [x] `performHandshake()` — START를 보내고 ACK 올 때까지 대기(타임아웃 300ms×5회 재시도),
      실패하면 데이터 전송 자체를 시작 안 함
- [ ] 배치(윈도우) 단위 ACK + 누락분만 재전송(`RETRANSMITTING`) — 미착수.
      오늘 실기기 테스트로 "청크마다 개별 ACK가 필요하다", "리시버가 먼저
      타임아웃 NACK을 보낼 수 있다"는 ESP32 쪽 실제 구현 방식을 확인해둠
      (`design-notes-FHSS-project-es.md` 5절) — 이 요구사항에 맞춰서 설계
- [ ] `PAUSED`/`FAILED`/`COMPLETED` 같은 명시적 상태 관리 — 미착수
- [ ] 위 로직들을 하나의 `OtaSession`(FSM) 클래스로 통합 — 미착수. 지금은
      `simplesender`/`simplereceiver`의 함수들을 CLI(`tests/smoke_*_main.cpp`)가
      순서대로 호출하는 구조라, 화면(`otamanager.cpp`)에서 쓰려면 상태 관리
      계층이 필요함
- [ ] `DISCOVER`/`DISCOVER_ACK` 기반 기기 탐색 흐름 — 프로토콜 레벨 타입/인코딩은
      `ota-protocol`에 이미 있음, `gateway-ota` 쪽 사용 로직은 미착수
- [ ] `otamanager.cpp`(화면)에 위 로직 연결 — 미착수

---

## 4. 마일스톤 5 (실기기 통합 검증, 라즈베리파이 2대) — 거의 완료

2026-08-15 라즈베리파이 2대(`pi24`=송신, `pi06`=수신)로 착수, 08-16에 정식
경로 전송 성공. 전체 경위는 `docs/note/design-notes-gateway-ota-es.md`
18~21절, 증상별 정리는
`kernel-cc1101-spi/docs/troubleshooting-cc1101.md` 참고.

### 검증 완료된 것 ✅

| 항목 | 결과 |
|---|---|
| 정식 경로(`Cc1101Transport`, `/dev/cc1101`) 파일 전송 | **1067/1067 청크, 디코딩 실패 0건, seq 0~1066 연속** |
| 핸드셰이크 → DATA → END 전체 흐름 | 완주, ACK 342개 수신 |
| 진단 경로(`SpidevTransport`) 파일 전송 | 1067/1067 (통제 실험 기준으로 유지) |
| 프로토콜 로직(분할/CRC16/핸드셰이크) | 실기기 정상 동작 확인 |

### 남은 것 — 파일 무결성 검증 🟡

지금까지 확인한 건 **"청크 개수"**입니다. 바이트 단위로 원본과 같은지는
아직 안 봤습니다. `tests/smoke_recv_main.cpp`에 재조립 기능을 넣어뒀으니
(2번째 인자에 출력 파일 경로) 다음 실기기 세션에서:

```sh
# 수신측
sudo ./ota_smoke_recv /dev/cc1101 recv.bin
# 양쪽에서 비교 — 이 두 해시가 같아야 진짜 성공
sha256sum test.bin      # 송신측
sha256sum recv.bin      # 수신측
```

**이게 마일스톤 5의 실질 완료 조건이자 `develop` 머지 게이트입니다.**
청크 수가 맞아도 오프셋이 밀렸거나 중복이 덮어썼으면 해시가 달라집니다.

### 이 과정에서 해결한 문제들

전부 원인이 규명되고 수정됐습니다 — 상세는 트러블슈팅 문서 참고.

| 문제 | 원인 계층 | 해결 |
|---|---|---|
| 수신 78.5%만 성공 | 소프트웨어 | `recv()`가 패킷 도착 완료 전에 FIFO를 읽던 버그 → 100% |
| GDO2 인터럽트 안 울림 | 하드웨어 | **수신측 안테나 불량** (교체) |
| 남의 패킷이 RX 큐를 채움 | 팀 차원 | 싱크워드를 OTA 전용 `0x2D/0xD4`로 분리 |
| RX 큐가 한번 차면 영구 마비 | 커널 | `open()` 시 flush + drop-oldest 정책 |
| 인터럽트 7,700만 회 폭주 | 커널 | 송신 완료 후 명시적 `SRX` 복원 |
| 하드웨어 주소필터가 패킷 폐기 | 칩 설정 | `PKTCTRL1`의 `ADR_CHK` 끔 (`0x0D`→`0x0C`) |
| 커널 헤더/소스 못 구해 모듈 적재 실패 | 인프라 | 빌드서버(10.10.16.54)에서 크로스컴파일로 해결 |

> **커널 드라이버(`kernel-cc1101-spi`)는 팀원3·4 담당 레포입니다.**
> 검증 과정에서 부득이 8군데를 고쳤고, 그 내역과 담당자 판단이 필요한 항목을
> `kernel-cc1101-spi/docs/driver-changes-handoff-2026-08-17.md`에 정리했습니다.
> **직접 머지하지 않고 PR로 리뷰 요청할 것** — 특히 임시 디버그 로그
> (`dev_warn`) 원복은 담당자 판단 사항입니다.

---

## 5. 다음에 할 일 (우선순위 순)

### 즉시 (`develop` 머지 전)

1. **`sha256sum` 파일 무결성 검증** — 마일스톤 5 완료 조건, 머지 게이트
2. `develop` 머지 + PR 문서화 (13커밋 누적, 싱크워드 변경은 팀 전체 영향)

### 다음 실질 작업 — 마일스톤 4 마무리

3. **배치 ACK + 누락분 재전송** 설계·구현. ESP32 쪽 실제 동작
   (청크마다 개별 ACK, 리시버 선제 타임아웃 NACK)에 맞춤 —
   `design-notes-FHSS-project-es.md` 5절
4. `simplesender`/`simplereceiver`를 **`OtaSession`(FSM)으로 통합**.
   지금은 CLI가 함수를 순서대로 호출하는 구조라, 화면에 붙이려면 상태 관리
   계층이 필요함 — 이게 마일스톤 4의 핵심 남은 덩어리
5. `otamanager.cpp`(화면)에 실제 전송 로직 연결

### 성능 / 팀 협의

6. **`chunkDelayMs`를 40ms 아래로** — 현재 1067청크에 약 43초. 10ms에서는
   패킷 경계가 밀림. `out_rearm`에서 `SFRX` 없이 `SRX`만 하는 것이 관련
   있을 수 있음(유저공간 구현은 매번 `SIDLE; SFRX; SRX`로 완전히 비움)
7. `ota-protocol`의 `OTA_BROADCAST_DEVICE_ID`(0xFFFFFFFF) vs
   `OTA_DEVICE_ID_MAX`(0xFFFFFF) 범위 모순 — 팀 합의 필요
8. **누가 어떤 주파수/싱크워드/채널을 쓰는지 팀 관리표 만들기** —
   이번 충돌 사고의 근본 원인
9. `DISCOVER`/`DISCOVER_ACK` 기반 기기 탐색 흐름 구현
10. ESP32 쪽 `main/fsm.c`의 OTA 수신 배선(TODO, 담당 "팀2") 완료 후
    실제 게이트웨이→ESP32 종단 간 테스트

---

## 참고 문서

- **`kernel-cc1101-spi/docs/troubleshooting-cc1101.md` — 증상별 트러블슈팅
  가이드. CC1101이 안 될 때 여기부터 보세요**
- `kernel-cc1101-spi/docs/driver-changes-handoff-2026-08-17.md` — 커널
  드라이버 변경 내역 + 담당자 리뷰 요청 사항
- `docs/testing-spidev-transport.md` — 진단 경로 빌드/실행법
- `OTA_System/tools/spidev/README.md` — 진단 도구의 존재 이유·삭제 조건
- `docs/note/pi-smoke-test-log-2026-08-15.md` — 라즈베리파이 실기기
  테스트 전체 과정/문제 기록 (멘토링 공유용)
- `docs/fsm-design.md` — OTA 송신 FSM 상세 설계 (상태/이벤트/전이표/다이어그램).
  **아직 커밋 안 됨** — 마일스톤 4의 `OtaSession` 통합 작업을 시작할 때 함께
  올릴 예정. 그때 설계와 실제 코드를 맞춰가며 고쳐야 하므로, 지금 올려두면
  구현 전 설계가 확정된 것처럼 보이는 게 부담이라 미룸
- `docs/note/design-notes-gateway-ota-es.md` — 개인 설계 노트 (전체 결정 이력)
- `docs/note/design-notes-FHSS-project-es.md` — 프로젝트 전체 그림 (3개 레포 관점)
- `ota-protocol/README.md` — 배치 확인 방식 등 팀 합의 필요 항목
- `firmware-esp32/fhss-ota-radio/docs/fsm-design.md` — 수신 측(ESP32) FSM
