# gateway-ota

OTA 매니저 Qt/C++ 앱(BIN 분할·전송·재전송).

## 핵심 기능
- `.bin` 파일 선택 및 청크 분할 (`ota-protocol` 규격)
- `/dev/cc1101` 경유 전송, 진행률·로그 표시
- ACK/NACK 기반 재전송 큐
- 유니캐스트(특정 단말) / 브로드캐스트(1:N) 전송 모드

> `ota-protocol` 핵심 스펙(패킷 구조/`version` 제거/DISCOVER)은
> 교차 확인까지 마쳤습니다(2026-08-11). 남은 합의 항목(`DISCOVER_ACK` 백오프 범위,
> `seq`/`total_chunks` 32bit 최종 확인)은 `ota-protocol` README 참고 — 이
> 상태기계 구조 자체에 영향을 주는 항목은 아닙니다.


## 담당
팀원3, 4

## 진행 상황

| 마일스톤 | 내용 | 상태 |
|---|---|---|
| 1 | Qt 프로젝트 세팅 및 화면 뼈대 | ✅ 완료 |
| 2 | 전송 계층 추상화 + `Cc1101Transport` | ✅ 완료 (실기기 1067/1067) |
| 3 | BIN 분할 + CRC | ✅ 완료 |
| 4 | 핸드셰이크 + 송수신 + ACK | 🟡 `OtaSession`(배치 ACK+선택적 재전송) 구현·유닛테스트 완료(2026-08-18) / **실기기 검증·화면 연결은 아직** |
| 5 | 실기기 통합 검증 | ✅ 완료 — 전송 + 재조립 무결성 검증 통과 |

> **무선 손실 약 0.75%는 재전송(마일스톤 4)으로 메워야 합니다.**
> 재조립 로직은 바이트 단위로 정확함이 검증됐지만, 재전송이 없으면
> 원본과 동일한 파일을 보장할 수 없습니다. 상세는 `docs/roadmap.md` 4절.

> **최신 상세 현황(파일 구성/마일스톤/다음 할 일)은
> [`docs/roadmap.md`](docs/roadmap.md) 참고** (2026-08-17 갱신).
> 아래 체크리스트는 예전 기록이라 일부 계획(`LocalFileTransport` 등)이
> 실제로는 다른 방식(`session/simplesender`·`simplereceiver`)으로 바뀌었습니다.

> **⚠️ 싱크워드 주의**: `SYNC1/SYNC0`이 OTA 전용값 `0x2D/0xD4`입니다
> (팀 공용 기본값 `0xD3/0x91`에서 변경 — 팀원들끼리 서로 패킷을 받는 문제가
> 있었음). `kernel-cc1101-spi/cc1101_core.c`의 `cc1101_default_regs[]`와
> **같은 값이어야** 통신됩니다.

### 마일스톤 1 — Qt 프로젝트 세팅 및 화면 뼈대
- [x] Qt 프로젝트 생성 (Widgets, CMake) — `OTA_System/`
- [x] 화면 뼈대 구현 (`OtaManager` : `ui/otamanager.h/.cpp`)
  - 연결(Transport) 카드 — 드라이버 선택(로컬 파일 / CC1101), 포트 입력, 연결 상태 표시
  - 전송 대상 카드 — 유니캐스트/브로드캐스트, 대상 ESP32 노드 선택
  - BIN 파일 카드 — 파일 선택, 파일 크기/청크 크기/총 청크 수 표시
  - 진행률 카드 — 진행률 바, ACK/재전송 카운트, 시작/일시정지 버튼
  - 로그 카드 — 타임스탬프 로그 뷰
- [x] 마지막 설정값(포트, 드라이버, 청크 크기, 전송모드) `QSettings` 저장/불러오기
- [ ] 로그창 실시간 전송 이벤트 연동 (마일스톤 3~4에서 실제 로직 연결 시)

> 참고: 버튼/필드는 현재 UI 골격만 동작하며, 연결·전송 로직은 각각 마일스톤 2(ITransport)·3(분할/CRC)·4(재전송 큐)에서 실제 구현으로 교체됩니다.
> 로컬 환경에 Qt6 + CMake가 설치돼 있어야 빌드/실행해서 화면을 확인할 수 있습니다 (개발 샌드박스에는 Qt가 없어 코드 리뷰만 진행함).

### 마일스톤 3 — BIN 분할 + CRC 구현 (착수)
- [x] `ota-protocol` 공유 레포 연동 — `core/otaprotocol.h`(자체 CRC32 버전)를 삭제하고,
      `gateway-ota`와 같은 상위 폴더에 형제 폴더로 clone된 `ota-protocol/include/ota_protocol.h`를 씀.
- [x] `BinSplitter` 클래스 — `core/binsplitter.h/.cpp`
  - `std::ifstream` 기반 청크 분할, `ota_protocol_encode_data()`로 헤더+payload 직렬화
  - 패딩 없음 — `payload_length` 필드가 실제 길이를 전달하므로 마지막 청크는 짧게 그대로 전송
  - **Qt 의존성 전혀 없음** (`std::string`/`std::vector` 등 표준 C++만 사용) → 화면 없이,
    Qt6/CMake 없이도 `g++`/`clang++`만으로 단독 컴파일·테스트 가능 (`transport/`도 동일)
  - **2026-08-11: `ota-protocol` v0.2 API로 갱신** — 헤더 12byte, 최대 payload
    48byte, CRC-16/CCITT-FALSE는 그대로지만 `session_id`(세션마다 랜덤,
    `OtaSession`이 만들어서 넘겨줌)가 새로 필요해져서 `split()` 시그니처에
    `sessionId` 파라미터 추가(`split(filePath, sessionId, chunkSize, errorMessage)`,
    기본값 없이 항상 명시). `OtaChunk::header` 타입도 `ota_packet_header_t`→
    `ota_data_header_fields_t`로 바뀌었고, `total_chunks`는 더 이상 DATA 헤더에
    없음(v0.2부터 `OTA_START`에만 실림 — 총 개수는 `split()` 결과 벡터의
    크기로 이미 알 수 있음). 헤더 값도 더 이상 와이어 바이트를 그대로
    `memcpy`하지 않고 이미 아는 값(session_id/sequence/payload_length)과
    `ota_protocol_crc16()` 결과로 직접 채움 (`ota_protocol.h` 자체의
    "구조체를 그대로 memcpy하지 않는다" 원칙과 동일). 자세한 이유는
    `docs/note/design-notes-gateway-ota-es.md` 13절
- [x] `ota_core` 정적 라이브러리로 분리 (`CMakeLists.txt`) — GUI 앱(`OTA_System`)과 유닛테스트(`ota_core_tests`)가 공통으로 링크. Qt를 전혀 링크하지 않음
- [x] `ota_core_tests` — `tests/tst_binsplitter.cpp` (assert 기반 순수 C++ 테스트, 창 없이 콘솔에서만 실행)
  - 정확히 나눠떨어지는 파일 / 짧은 마지막 청크 / CRC 무결성(디코딩 거부) / 에러 케이스 /
    쪼갠 뒤 다시 합쳐서 원본과 바이트 단위로 일치하는지 확인하는 round-trip 테스트,
    7개 전부 통과(g++ -std=c++17 확인)
  - `g++ -std=c++17 -I. -Icore -I../../ota-protocol/include tests/tst_binsplitter.cpp core/binsplitter.cpp -o tst_binsplitter && ./tst_binsplitter`로 CMake 없이도 바로 실행 가능
- [ ] BinSplitter 결과를 `otamanager.cpp`(화면)에서 실제로 호출해 진행률·로그에 반영
- [ ] `LocalFileTransport`(ITransport 임시 구현체, 로컬 파일 write)로 직렬화 결과 저장

> **빌드 전 필수**: `ota-protocol` 레포를 `gateway-ota`와 같은 상위 폴더에 clone해두세요
> (`git clone git@github.com:fhss-ota-radio/ota-protocol.git`, `gateway-ota` 옆에 나란히).
> 다른 위치에 두셨다면 `cmake -B build -DOTA_PROTOCOL_INCLUDE_DIR=경로/include`로 지정
> (안 맞으면 CMake 설정 단계에서 안내 메시지와 함께 에러).
> 빌드: `cmake --build . --target ota_core_tests && ctest -R ota_core_tests` (GUI 앱 `OTA_System`은 안 띄우고 로직만 검증 가능)

### 마일스톤 2 — 전송 계층 추상화 (완료)
- [x] `ITransport` 인터페이스 — `transport/itransport.h` (open/close/isOpen/send/recv)
- [x] `Cc1101Status`/`Cc1101RxMetadata` — `transport/cc1101_status.h` (통신 팀원 4·5의 `cc1101-radio-api.md`와 의미 통일)
- [x] `Cc1101Transport` 실구현 — `transport/cc1101transport.h/.cpp` (POSIX `open(O_NONBLOCK)/write/poll+read/ioctl`), `transport/cc1101_ioctl.h`(UAPI 계약 헤더) 추가. `#if defined(__linux__)`로 감싸서 리눅스(라즈베리파이)에서만 실구현이 빌드되고, macOS 등 로컬 환경에서는 자동으로 안전한 폴백 스텁이 빌드됨(로컬 빌드 안 깨짐)
  - **[검증 완료, 2026-08-16] 실기기 2대로 51200byte / 1067청크 전량 수신 성공**
    (1067/1067, 디코딩 실패 0건). 핸드셰이크 → DATA → END 전체 흐름 완주.
    한동안 "GDO2 인터럽트가 안 울린다"며 막혀 있었으나 원인은 ①수신측 안테나
    불량 ②팀원들과 싱크워드가 겹쳐 남의 트래픽이 커널 RX 큐를 채운 것
    ③송신 완료 후 RX 재진입 처리였음 — 전부 해결.
    자세한 경위는 `docs/note/design-notes-gateway-ota-es.md` 18~21절,
    `kernel-cc1101-spi/docs/driver-changes-handoff-2026-08-17.md` 참고.
    - 현재 송신 시 청크 간 대기 **40ms** 필요(`chunkDelayMs`). 10ms에서는
      수신이 못 따라가 패킷 경계가 밀림 — 수신 처리량 개선은 남은 과제.
- [x] 폴더 재구성 — `ui/`(화면) · `core/`(분할·CRC·프로토콜) · `transport/`(ITransport·CC1101) · `tests/`
- [x] **(2026-08-17)** `SpidevTransport`를 진단 도구로 재분류 —
      [`OTA_System/tools/spidev/`](OTA_System/tools/spidev/README.md).
      커널 드라이버가 막혀 있던 동안의 우회로였으나, 정식 경로가 열린 뒤에도
      **커널 계층을 우회하는 통제 실험(control experiment) 도구**로 남겼습니다.
      "안 되는 원인이 하드웨어냐 커널이냐"를 한 번에 가를 수 있어서, 실제로
      2026-08-16 디버깅에서 결정적이었습니다.
      제품 라이브러리(`ota_core`)에는 **의도적으로 넣지 않았습니다** — 폴더
      하나만 지우면 제거되도록. 존재 이유·삭제 조건은 위 README,
      빌드/실행법은
      [`docs/testing-spidev-transport.md`](docs/testing-spidev-transport.md) 참고.

### 마일스톤 4 — `OtaSession`(FSM) — 배치 ACK + 선택적 재전송 (진행 중)
- [x] **(2026-08-18)** `session/otasession.h/.cpp` — [`docs/fsm-design.md`](docs/fsm-design.md)의
      송신측 상태기계 구현체. `HANDSHAKING`→`SENDING_BATCH`→`WAITING_BATCH_ACK`↔`RETRANSMITTING`→`WAITING_END_ACK`→`COMPLETED`/`FAILED`,
      `PAUSED` 포함. `simplesender`/`simplereceiver`의 START/END 인코딩·수신 디코딩을
      그대로 재사용
  - 배치(기본 5청크) 단위로 전부 전송 후 확인 대기, 슬롯(청크)별 개별
    타임아웃(기본 300ms)·재시도(기본 5회) 관리. NACK은 타임아웃을 기다리지
    않고 그 슬롯만 즉시 재전송(Selective-Repeat, 배치 전체 재전송 아님)
  - `tick(nowMs)`를 시각 파라미터로 받는 구조 — Qt `QTimer`로 주기 호출하면
    되고, 테스트에서는 가짜 시각을 넣어 타임아웃을 실제로 기다리지 않고 검증
  - `tests/tst_otasession.cpp` — `FakeTransport`(인메모리)로 핸드셰이크/배치
    ACK/NACK 즉시재전송/타임아웃재전송/재시도초과/END NACK/일시정지-재개 7개
    시나리오 전부 통과 (g++ -std=c++17 확인, ctest 등록됨)
- [ ] **실기기(라즈베리파이 2대) 검증 — 아직.** `simplereceiver.cpp`의
      `sendAckFor()`가 현재 NACK을 실제로 안 보내서(항상 ACK만 생성), NACK
      경로는 시뮬레이션으로만 확인됨
- [ ] `DISCOVER`/`DISCOVER_ACK` 기기 조회 — `OtaSession`에 의도적으로 미포함
      (연결·화면 담당 상위 흐름, `docs/roadmap.md` 3절 참고)
- [ ] `otamanager.cpp`(화면)에 연결 — 미착수

## 문서

| 문서 | 언제 보나 |
|---|---|
| [`docs/quickstart-two-pi-test.md`](docs/quickstart-two-pi-test.md) | **처음 받았을 때 — 클론부터 실기기 전송까지 따라하기** |
| [`docs/roadmap.md`](docs/roadmap.md) | 현재 진행 상황·다음 할 일 |
| [`docs/file-transfer-guide.md`](docs/file-transfer-guide.md) | 파일 전송이 코드 안에서 어떻게 도는지 |
| [`docs/testing-spidev-transport.md`](docs/testing-spidev-transport.md) | 진단 경로 빌드/실행법 |
| `kernel-cc1101-spi/docs/troubleshooting-cc1101.md` | **CC1101이 안 될 때** |
| `kernel-cc1101-spi/docs/driver-changes-handoff-2026-08-17.md` | 커널 드라이버 변경 내역 (담당자용) |

