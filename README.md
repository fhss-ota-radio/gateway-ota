# gateway-ota

OTA 매니저 Qt/C++ 앱(BIN 분할·전송·재전송).

## 핵심 기능
- `.bin` 파일 선택 및 청크 분할 (`ota-protocol` 규격)
- `/dev/cc1101` 경유 전송, 진행률·로그 표시
- ACK/NACK 기반 재전송 큐
- 유니캐스트(특정 단말) / 브로드캐스트(1:N) 전송 모드

## 담당
팀원3, 4

## 진행 상황

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
      헤더 9byte, 최대 payload 55byte, CRC-16/CCITT-FALSE (자세한 이유는 `docs/design-notes-gateway-ota-es.md` 9절)
- [x] `BinSplitter` 클래스 — `core/binsplitter.h/.cpp`
  - QFile 기반 청크 분할, `ota_protocol_encode_data()`로 헤더+payload 직렬화
  - 패딩 없음 — `payload_length` 필드가 실제 길이를 전달하므로 마지막 청크는 짧게 그대로 전송
  - QWidget/QMainWindow 의존성 없음 → 화면 없이 단독 테스트 가능
- [x] `ota_core` 정적 라이브러리로 분리 (`CMakeLists.txt`) — GUI 앱(`OTA_System`)과 유닛테스트(`ota_core_tests`)가 공통으로 링크
- [x] `ota_core_tests` — `tests/tst_binsplitter.cpp` (`QTEST_APPLESS_MAIN`, 창 없이 콘솔에서만 실행)
  - 정확히 나눠떨어지는 파일 / 짧은 마지막 청크 / CRC 무결성(디코딩 거부) / 에러 케이스
- [ ] BinSplitter 결과를 `otamanager.cpp`(화면)에서 실제로 호출해 진행률·로그에 반영
- [ ] `LocalFileTransport`(ITransport 임시 구현체, 로컬 파일 write)로 직렬화 결과 저장

> **빌드 전 필수**: `ota-protocol` 레포를 `gateway-ota`와 같은 상위 폴더에 clone해두세요
> (`git clone git@github.com:fhss-ota-radio/ota-protocol.git`, `gateway-ota` 옆에 나란히).
> 다른 위치에 두셨다면 `cmake -B build -DOTA_PROTOCOL_INCLUDE_DIR=경로/include`로 지정
> (안 맞으면 CMake 설정 단계에서 안내 메시지와 함께 에러).
> 빌드: `cmake --build . --target ota_core_tests && ctest -R ota_core_tests` (GUI 앱 `OTA_System`은 안 띄우고 로직만 검증 가능)

### 마일스톤 2 — 전송 계층 추상화 (착수)
- [x] `ITransport` 인터페이스 — `transport/itransport.h` (open/close/isOpen/send/recv)
- [x] `Cc1101Status`/`Cc1101RxMetadata` — `transport/cc1101_status.h` (통신 팀원 4·5의 `cc1101-radio-api.md`와 의미 통일)
- [x] `Cc1101Transport` 스켈레톤 — `transport/cc1101transport.h/.cpp` (지금은 open()이 항상 실패하는 스텁, 부품 입고 후 POSIX `open/write/poll+read/ioctl`로 구현 예정)
- [x] 폴더 재구성 — `ui/`(화면) · `core/`(분할·CRC·프로토콜) · `transport/`(ITransport·CC1101) · `tests/`
- [ ] 팀원 4·5 커널 드라이버(`/dev/cc1101`) 완성 후 `Cc1101Transport` 실제 구현

> 자세한 배경(왜 커널 모듈은 C인지, CC1101 라이브러리 담당 범위, 팀 합의 필요 항목 등)은
> `docs/design-notes-gateway-ota-es.md` 참고 (개인 참고용 문서라 `.gitignore`에 있어 이 레포를
> 새로 clone한 팀원 화면에는 안 보일 수 있습니다).
