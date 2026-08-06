cmake_minimum_required(VERSION 3.19)
project(OTA_System LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Test)

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

# ---------- ota_core: 화면(QWidget/QMainWindow)과 무관한 순수 로직 ----------
# core/      BIN 분할 (ota-protocol의 공유 헤더로 패킷 직렬화)
# transport/ ITransport 추상화 + CC1101 구현체(사용자 공간 래퍼)
# GUI 앱과 유닛테스트 양쪽에서 이 라이브러리를 그대로 링크해서 재사용한다.
add_library(ota_core STATIC
    core/binsplitter.h
    core/binsplitter.cpp
    transport/itransport.h
    transport/cc1101_status.h
    transport/cc1101transport.h
    transport/cc1101transport.cpp
)
target_link_libraries(ota_core PUBLIC Qt::Core)
target_include_directories(ota_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/transport
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

# ---------- ota_core_tests: 화면 없이 core/transport 로직만 독립 실행/검증 ----------
enable_testing()

qt_add_executable(ota_core_tests
    tests/tst_binsplitter.cpp
)
target_link_libraries(ota_core_tests
    PRIVATE
        Qt::Core
        Qt::Test
        ota_core
)
add_test(NAME ota_core_tests COMMAND ota_core_tests)
