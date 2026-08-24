cmake_minimum_required(VERSION 3.19)
project(OTA_System LANGUAGES CXX)

# Qt::Test는 더 이상 안 씀 — ota_core_tests가 Qt 의존성 없는 순수 C++ 테스트로 바뀌었음
# (core/·transport/에서 Qt 타입을 제거한 결정, docs/note/design-notes-gateway-ota-es.md 참고)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)

qt_standard_project_setup()

# ---------- ota-protocol: firmware-esp32와 공유하는 OTA 패킷 규격 ----------
# 순수 C, header-only라 링크 대상 없이 include 경로만 있으면 됨.
#
# gateway-ota와 ota-protocol은 "같은 상위 폴더" 밑에 나란히 있다고 가정합니다:
#   (상위 폴더)/
#     gateway-ota/    <- 이 레포
#     ota-protocol/   <- 형제 폴더
#
# 지금은 이 상위 폴더가 그냥 로컬 폴더지만, 나중에 이 상위 폴더 자체를 git
# 레포(최상위 레포)로 만들고 gateway-ota·ota-protocol을 그 밑에 git submodule로
# 넣더라도 물리적 폴더 배치(형제 폴더 관계)는 똑같이 유지되므로 이 경로를
# 다시 안 고쳐도 됩니다 — submodule이든 아니든 "위치"는 같기 때문입니다.
set(OTA_PROTOCOL_INCLUDE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ota-protocol/include
    CACHE PATH "ota-protocol include 경로 (기본값: gateway-ota와 같은 상위 폴더)")

if(NOT EXISTS ${OTA_PROTOCOL_INCLUDE_DIR}/ota_protocol.h)
    message(FATAL_ERROR
        "ota-protocol을 못 찾았습니다: ${OTA_PROTOCOL_INCLUDE_DIR}\n"
        "gateway-ota와 같은 상위 폴더에 ota-protocol 레포를 두세요 (직접 clone 또는\n"
        "상위 폴더를 git 레포로 만들어 git submodule로 추가). 위치가 다르면\n"
        "-DOTA_PROTOCOL_INCLUDE_DIR=경로/include 로 직접 지정할 수 있습니다.")
endif()
message(STATUS "ota-protocol include: ${OTA_PROTOCOL_INCLUDE_DIR}")

# ---------- ota_core: 화면(QWidget/QMainWindow) + Qt 전체와 무관한 순수 로직 ----------
# core/      BIN 분할 (ota-protocol의 공유 헤더로 패킷 직렬화) — 순수 데이터 변환, I/O 없음
# transport/ ITransport 추상화 + CC1101 구현체(사용자 공간 래퍼) — I/O 담당
# session/   core/·transport/를 엮어서 실제 "전송 흐름"을 만드는 조율(orchestration) 계층.
#            지금은 simplesender(ACK 없는 단순 전송)뿐이지만, 다음 단계 OtaSession
#            (FSM, ACK/재전송)도 이 폴더에 들어갈 예정 — core/에 안 둔 이유는
#            core/가 "순수 변환 로직" 전용 자리라는 정의를 유지하고 싶어서
#            (docs/note/design-notes-gateway-ota-es.md 14절 참고)
#
# core/·transport/·session/ 전부 std::string/std::vector 등 표준 C++만 쓰고 Qt 타입을
# 의도적으로 안 씁니다 — 그래서 ota_core는 Qt를 전혀 링크하지 않습니다.
# (ota-protocol처럼 g++/clang++만으로도 빌드·테스트 가능하게 하려는 목적,
# docs/note/design-notes-gateway-ota-es.md 참고)
#
# GUI 앱과 유닛테스트 양쪽에서 이 라이브러리를 그대로 링크해서 재사용한다.
add_library(ota_core STATIC
    core/binsplitter.h
    core/binsplitter.cpp
    core/sha256.h
    core/sha256.cpp
    core/hopseed.h
    core/hopseed.cpp
    session/simplesender.h
    session/simplesender.cpp
    session/simplereceiver.h
    session/simplereceiver.cpp
    session/otasession.h
    session/otasession.cpp
    session/discovery.h
    session/discovery.cpp
    session/fhssrollout.h
    session/fhssrollout.cpp
    transport/itransport.h
    transport/cc1101_status.h
    transport/cc1101transport.h
    transport/cc1101transport.cpp
)
target_compile_features(ota_core PUBLIC cxx_std_17)
target_include_directories(ota_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/transport
    ${CMAKE_CURRENT_SOURCE_DIR}/session
    ${OTA_PROTOCOL_INCLUDE_DIR}
)

# ---------- OTA_System: GUI 앱 ----------
qt_add_executable(OTA_System
    WIN32 MACOSX_BUNDLE
    main.cpp
    ui/otamanager.cpp
    ui/otamanager.h
    ui/otamanager.ui
)

target_link_libraries(OTA_System
    PRIVATE
        Qt::Core
        Qt::Widgets
        ota_core
)

install(TARGETS OTA_System
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET OTA_System
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# ---------- ota_core_tests: Qt 없이 core/transport 로직만 독립 실행/검증 ----------
# ota_core가 Qt를 링크 안 하므로 이 테스트도 qt_add_executable이 아닌 일반
# add_executable로 충분함 (Qt::Test도 필요 없음).
enable_testing()

add_executable(ota_core_tests
    tests/tst_binsplitter.cpp
)
target_link_libraries(ota_core_tests
    PRIVATE
        ota_core
)
add_test(NAME ota_core_tests COMMAND ota_core_tests)

# ---------- ota_session_tests: OtaSession(FSM) 배치ACK/재전송 로직 검증 ----------
# tst_binsplitter.cpp와 별개 실행파일로 둔 이유: 기존 관례(파일 하나 = main() 하나 =
# 실행파일 하나)를 그대로 따름. FakeTransport(인메모리)로 실기기 없이 검증하므로
# ota_core_tests와 마찬가지로 add_test()에 등록해 ctest로 자동 실행됨.
add_executable(ota_session_tests
    tests/tst_otasession.cpp
)
target_link_libraries(ota_session_tests
    PRIVATE
        ota_core
)
add_test(NAME ota_session_tests COMMAND ota_session_tests)

# ---------- ota_receiver_tests: sendAckFor()/peekDataHeaderForNack() 검증 ----------
# 2026-08-19 발견: sendAckFor()가 resultCode와 무관하게 항상 OTA_PKT_ACK로만
# 인코딩하던 버그(NACK이 와이어에 한 번도 안 나갔음)의 회귀 테스트.
add_executable(ota_receiver_tests
    tests/tst_simplereceiver.cpp
)
target_link_libraries(ota_receiver_tests
    PRIVATE
        ota_core
)
add_test(NAME ota_receiver_tests COMMAND ota_receiver_tests)

# ---------- ota_discovery_tests: discoverDevices() (DISCOVER/DISCOVER_ACK) 검증 ----------
# 2026-08-20 착수. FakeTransport(인메모리)로 실기기 없이 조회/중복제거/무응답
# 시나리오를 검증 — ota_session_tests와 같은 패턴.
add_executable(ota_discovery_tests
    tests/tst_discovery.cpp
)
target_link_libraries(ota_discovery_tests
    PRIVATE
        ota_core
)
add_test(NAME ota_discovery_tests COMMAND ota_discovery_tests)

# ---------- ota_fhssrollout_tests: rolloutFhssConfig()(FHSS_CONFIG/ACTIVATE) 검증 ----------
# 2026-08-22 착수. FakeTransport(인메모리)로 실기기 없이 성공/무응답 재시도
# 실패/부분 실패(기기 여러 대 중 하나만 실패)/NACK 재시도 시나리오를 검증 —
# ota_discovery_tests와 같은 패턴. 배경: docs/note/design-notes-gateway-ota-es.md
# (gateway-ota 담당 FHSS 항목, firmware-esp32 develop 725b27d 대응).
add_executable(ota_fhssrollout_tests
    tests/tst_fhssrollout.cpp
)
target_link_libraries(ota_fhssrollout_tests
    PRIVATE
        ota_core
)
add_test(NAME ota_fhssrollout_tests COMMAND ota_fhssrollout_tests)

# ---------- ota_hopseed_tests: deriveSessionHopSeed()/scanSecretSeedFolder()(core/hopseed.h) 검증 ----------
# firmware-esp32(fhss_audio_adapter.c derive_session_hop_seed())와 같은
# HMAC-SHA256 파생값을 계산하는지 알려진 테스트 벡터로 확인 — 두 기기가
# 다른 hop_seed를 계산하면 홉이 어긋나 통신 자체가 안 되므로 회귀 시
# 치명적이라 별도 타깃으로 분리.
add_executable(ota_hopseed_tests
    tests/tst_hopseed.cpp
)
target_link_libraries(ota_hopseed_tests
    PRIVATE
        ota_core
)
add_test(NAME ota_hopseed_tests COMMAND ota_hopseed_tests)

# ---------- ota_smoke_send / ota_smoke_recv: 실기기(CC1101) 스모크테스트용 CLI ----------
# /dev/cc1101이 실제로 있어야 동작하는 수동 실행 도구라서 add_test()에는 등록하지
# 않음 — ctest(자동 빌드 검증)에는 안 끼고, 실기기 앞에서 사람이 직접 실행.
add_executable(ota_smoke_send
    tests/smoke_send_main.cpp
)
target_link_libraries(ota_smoke_send
    PRIVATE
        ota_core
)

add_executable(ota_smoke_recv
    tests/smoke_recv_main.cpp
)
target_link_libraries(ota_smoke_recv
    PRIVATE
        ota_core
)

# OtaSession(FSM, 배치 ACK+선택적 재전송) 기반 송신 CLI — ota_smoke_send와
# 하는 일은 같지만 안쪽 로직이 session/otasession.h를 씀. 수신측은 별도
# 프로그램 없이 기존 ota_smoke_recv를 그대로 씀(이미 청크마다 개별 ACK를
# 반사적으로 보내고 있어서 OtaSession이 기대하는 응답과 맞음).
add_executable(ota_smoke_session_send
    tests/smoke_session_send_main.cpp
)
target_link_libraries(ota_smoke_session_send
    PRIVATE
        ota_core
)

# session/simplereceiver.h의 sendAckFor()가 실제로 NACK을 만들어 보내는지
# 실기기(CC1101)로 확인하는 전용 도구 — 일부러 CRC를 깨뜨리거나 범위 밖
# sequence로 DATA를 보내서, ota_smoke_recv가 진짜 NACK을 돌려주는지 본다.
# 자세한 이유는 tests/smoke_bad_data_main.cpp 상단 주석 참고.
add_executable(ota_smoke_bad_data
    tests/smoke_bad_data_main.cpp
)
target_link_libraries(ota_smoke_bad_data
    PRIVATE
        ota_core
)

# discoverDevices()(session/discovery.h)로 실제 ESP32를 조회해보는 CLI —
# 2026-08-20. ESP32가 MENU_OTA 화면일 때만 DISCOVER_ACK로 응답한다
# (firmware-esp32의 ota_consumer_is_ota_mode() 조건).
add_executable(ota_smoke_discover
    tests/smoke_discover_main.cpp
)
target_link_libraries(ota_smoke_discover
    PRIVATE
        ota_core
)

# DISCOVER -> explicit device selection -> targeted OtaSession START/DATA/END.
# Hardware-only and bounded by the CLI's max_session_ms argument, so it is not
# registered with ctest.
add_executable(ota_smoke_discover_session_send
    tests/smoke_discover_session_send_main.cpp
)
target_link_libraries(ota_smoke_discover_session_send
    PRIVATE
        ota_core
)

# rolloutFhssConfig()(session/fhssrollout.h)로 실제 ESP32에 FHSS 설정을
# 배포·활성화하고, Gateway 자신도 커널 호핑(MASTER)을 켜서 동기화 상태를
# 지켜보는 CLI — 2026-08-22. RF 프로필 기본값에 대한 주의는
# smoke_fhss_activate_main.cpp 상단 주석 참고(팀 재확인 필요한 추정치).
add_executable(ota_smoke_fhss_activate
    tests/smoke_fhss_activate_main.cpp
)
target_link_libraries(ota_smoke_fhss_activate
    PRIVATE
        ota_core
)

# CONFIG/ACTIVATE/커널 호핑 시작까지는 ota_smoke_fhss_activate와 동일하게 하고,
# 그 위에서 OtaSession으로 실제 파일 전송까지 이어붙이는 CLI — 2026-08-23.
# ota_smoke_fhss_activate가 SYNC 관찰까지만 하고 끝나는 도구라는 게 확인된 뒤
# (design-notes-gateway-ota-es.md 42절 5차 시도), 그 다음 단계로 추가함.
add_executable(ota_smoke_fhss_ota_transfer
    tests/smoke_fhss_ota_transfer_main.cpp
)
target_link_libraries(ota_smoke_fhss_ota_transfer
    PRIVATE
        ota_core
)

# ---------- tools/spidev: 진단 도구 (제품 코드 아님) ----------
# SpidevTransport는 커널 드라이버(/dev/cc1101)를 우회해서 /dev/spidevX.Y로 CC1101을
# 직접 폴링하는 ITransport 구현체입니다. "안 되는 원인이 하드웨어냐 커널이냐"를
# 가르는 통제 실험(control experiment) 용도이며, 정식 전송 경로가 아닙니다.
#
# **의도적으로 ota_core에 넣지 않았습니다.** ota_core는 GUI 앱(OTA_System)과
# 유닛테스트가 함께 링크하는 제품 라이브러리인데, 앱이 절대 쓰지 않는 진단
# 코드를 거기 넣으면 "제품에 포함된 코드"처럼 보이고 나중에 지우기도 어려워집니다.
# 그래서 아래 두 실행파일이 spidevtransport.cpp를 직접 컴파일합니다 —
# 이 폴더를 삭제하고 이 블록만 지우면 깔끔하게 제거됩니다.
#
# 존재 이유·사용 시점·삭제 조건은 tools/spidev/README.md 참고.
# 실기기 전용 수동 도구라 add_test()에는 등록하지 않습니다.
add_executable(ota_smoke_spidev_send
    tools/spidev/smoke_spidev_send_main.cpp
    tools/spidev/spidevtransport.cpp
)
target_link_libraries(ota_smoke_spidev_send
    PRIVATE
        ota_core
)
target_include_directories(ota_smoke_spidev_send
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/spidev
)

add_executable(ota_smoke_spidev_recv
    tools/spidev/smoke_spidev_recv_main.cpp
    tools/spidev/spidevtransport.cpp
)
target_link_libraries(ota_smoke_spidev_recv
    PRIVATE
        ota_core
)
target_include_directories(ota_smoke_spidev_recv
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/spidev
)
