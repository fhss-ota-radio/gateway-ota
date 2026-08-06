cmake_minimum_required(VERSION 3.19)
project(OTA_System LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)

qt_standard_project_setup()

qt_add_executable(OTA_System
    WIN32 MACOSX_BUNDLE
    main.cpp
    otamanager.cpp
    otamanager.h
    otamanager.ui
)

target_link_libraries(OTA_System
    PRIVATE
        Qt::Core
        Qt::Widgets
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
