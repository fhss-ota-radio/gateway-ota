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
    session/simplesender.h
    session/simplesender.cpp
    session/simplereceiver.h
    session/simplereceiver.cpp
    transport/itransport.h
    transport/cc1101_status.h
    transport/cc1101transport.h
    transport/cc1101transport.cpp
    transport/spidevtransport.h
    transport/spidevtransport.cpp
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

# ---------- ota_smoke_spidev_send / ota_smoke_spidev_recv: 커널 드라이버 우회용 ----------
# smoke_send/smoke_recv와 로직은 동일하되 Cc1101Transport(/dev/cc1101, 커널
# 드라이버 필요) 대신 SpidevTransport(/dev/spidevX.Y, 커널 드라이버 불필요)를
# 씀. 자세한 사용법은 docs/testing-spidev-transport.md 참고. 이것도 실기기
# 전용 수동 도구라 add_test()에는 등록하지 않음.
add_executable(ota_smoke_spidev_send
    tests/smoke_spidev_send_main.cpp
)
target_link_libraries(ota_smoke_spidev_send
    PRIVATE
        ota_core
)

add_executable(ota_smoke_spidev_recv
    tests/smoke_spidev_recv_main.cpp
)
target_link_libraries(ota_smoke_spidev_recv
    PRIVATE
        ota_core
)
