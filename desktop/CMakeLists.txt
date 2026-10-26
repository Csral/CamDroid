cmake_minimum_required(VERSION 3.20)
project(CamDroidDesktop VERSION 1.0 LANGUAGES CXX)
set(CAMDROID_RELEASE_VERSION "${PROJECT_VERSION}")

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

include(CTest)

find_package(FFMPEG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(Threads REQUIRED)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../protocol"
    "${CMAKE_CURRENT_BINARY_DIR}/protocol"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(camdroid_core STATIC
    src/application.cpp
    src/tcp_socket.cpp
    src/video_decoder.cpp
)

target_include_directories(camdroid_core PUBLIC ${FFMPEG_INCLUDE_DIRS})
target_link_directories(camdroid_core PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(camdroid_core PUBLIC ${FFMPEG_LIBRARIES})

target_link_libraries(camdroid_core PUBLIC camdroid_protocol)
target_link_libraries(camdroid_core PUBLIC Threads::Threads)

target_include_directories(camdroid_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/includes
)

if(WIN32)
    target_compile_definitions(camdroid_core PUBLIC NOMINMAX)
    target_link_libraries(camdroid_core PRIVATE ws2_32 iphlpapi)
endif()

add_library(camdroid_webcam STATIC src/virtual_camera.cpp)
target_include_directories(camdroid_webcam PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/webcam")
target_link_libraries(camdroid_webcam PUBLIC camdroid_core Qt6::Gui)

if(WIN32)
    enable_language(RC)
    add_executable(camdroid_uninstall WIN32 installer/uninstall.cpp installer/branding.rc)
    target_include_directories(camdroid_uninstall PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    target_link_libraries(camdroid_uninstall PRIVATE msi shell32)
    set_property(TARGET camdroid_uninstall PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_options(camdroid_uninstall PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
    add_library(camdroid_setup_actions SHARED installer/actions.cpp)
    target_include_directories(
        camdroid_setup_actions
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/webcam"
        "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/includes"
    )
    target_link_libraries(camdroid_setup_actions PRIVATE msi shell32 ole32 advapi32)
    set_property(TARGET camdroid_setup_actions PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(camdroid_webcam PRIVATE mfplat mf mfuuid ole32 advapi32 shell32)
    add_library(camdroid_camera SHARED webcam/camera_source.cpp webcam/camera_source.def)
    target_include_directories(
        camdroid_camera
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/webcam"
        "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/includes"
    )
    target_compile_definitions(camdroid_camera PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(camdroid_camera PRIVATE mfplat mf mfuuid mfsensorgroup ole32 advapi32 shell32 uuid)
    set_property(TARGET camdroid_camera PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(camdroid_desktop
    src/main.cpp
    src/cli.cpp
    src/preview.cpp
)

qt_add_resources(camdroid_desktop camdroid_branding
    PREFIX "/branding"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/camdroid.png"
    OPTIONS --no-compress
)

if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS camdroid_desktop RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES assets/camdroid.png DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/1024x1024/apps")
    install(FILES assets/camdroid.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
endif()

target_link_libraries(camdroid_desktop PRIVATE
    camdroid_core
    camdroid_webcam
    Qt6::Widgets
    Qt6::Network
    Threads::Threads
)

if(WIN32)
    target_sources(camdroid_desktop PRIVATE assets/app.rc)
    target_include_directories(camdroid_desktop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_dependencies(camdroid_desktop camdroid_camera)
    target_link_options(camdroid_desktop PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    add_executable(camdroid_console src/console_launcher.cpp assets/app.rc)
    target_include_directories(camdroid_console PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    set_target_properties(camdroid_console PROPERTIES OUTPUT_NAME camdroid_desktop SUFFIX ".com"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_custom_command(TARGET camdroid_desktop POST_BUILD
        COMMAND Qt6::windeployqt
            "$<IF:$<CONFIG:Debug>,--debug,--release>"
            --no-translations
            --dir "$<TARGET_FILE_DIR:camdroid_desktop>"
            "$<TARGET_FILE:camdroid_desktop>"
        COMMENT "Deploying Qt runtime libraries and plugins"
        VERBATIM
    )

    find_program(CAMDROID_WINDOWS_POWERSHELL NAMES powershell
        PATHS "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0" NO_DEFAULT_PATH REQUIRED)
    set(CAMDROID_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist" CACHE PATH "Installer output directory")
    set(package_output "${CAMDROID_PACKAGE_DIRECTORY}/$<CONFIG>")
    add_custom_command(
        OUTPUT "${package_output}/CamDroid-Setup.exe" "${package_output}/CamDroid-Uninstall.exe"
            "${package_output}/CamDroid-${CAMDROID_RELEASE_VERSION}-x64.msi"
        COMMAND "${CAMDROID_WINDOWS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/installer/build.ps1"
            -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}"
            -RuntimeDirectory "$<TARGET_FILE_DIR:camdroid_desktop>"
            -Configuration "$<CONFIG>"
            -Version "${CAMDROID_RELEASE_VERSION}"
            -InstallerVersion "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.0"
            -VcpkgInstalled "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
            -OutputDirectory "${package_output}"
        DEPENDS camdroid_desktop camdroid_console camdroid_uninstall camdroid_camera camdroid_setup_actions
            "$<TARGET_FILE:camdroid_console>"
            "$<TARGET_FILE:camdroid_desktop>" "$<TARGET_FILE:camdroid_uninstall>"
            "$<TARGET_FILE:camdroid_camera>" "$<TARGET_FILE:camdroid_setup_actions>"
            installer/build.ps1 installer/Package.wxs installer/Bundle.wxs installer/Theme.xml assets/camdroid.ico assets/camdroid.png
            "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/../app/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/COPYING"
        COMMENT "Packaging CamDroid setup, MSI and uninstaller"
        VERBATIM
    )
    add_custom_target(camdroid_installer ALL DEPENDS
        "${package_output}/CamDroid-Setup.exe" "${package_output}/CamDroid-Uninstall.exe"
        "${package_output}/CamDroid-${CAMDROID_RELEASE_VERSION}-x64.msi")
endif()

if(BUILD_TESTING)
    if(WIN32)
        add_executable(camdroid_webcam_system_test tests/webcam_system_test.cpp)
        target_include_directories(camdroid_webcam_system_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/webcam")
        target_link_libraries(camdroid_webcam_system_test PRIVATE camdroid_webcam mfsensorgroup mfreadwrite)
        add_executable(camdroid_installer_test tests/installer_test.cpp)
        target_include_directories(
            camdroid_installer_test
            PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/webcam"
            "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/includes"
        )
        target_link_libraries(camdroid_installer_test PRIVATE ole32 shell32 advapi32)
        add_test(NAME desktop_installer COMMAND camdroid_installer_test)
        add_executable(camdroid_webcam_policy_test tests/webcam_policy_test.cpp)
        target_include_directories(
            camdroid_webcam_policy_test
            PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/webcam"
            "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/includes"
        )
        target_link_libraries(camdroid_webcam_policy_test PRIVATE mfplat mfreadwrite mfuuid ole32 advapi32 shell32)
        add_dependencies(camdroid_webcam_policy_test camdroid_camera)
        add_test(NAME desktop_webcam_policy COMMAND camdroid_webcam_policy_test "$<TARGET_FILE:camdroid_camera>")
        set_tests_properties(desktop_webcam_policy PROPERTIES TIMEOUT 20)
        add_executable(camdroid_webcam_test tests/webcam_test.cpp)
        target_include_directories(
            camdroid_webcam_test
            PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/webcam"
            "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/includes"
        )
        target_link_libraries(camdroid_webcam_test PRIVATE mfplat mfreadwrite mfuuid ole32 advapi32 shell32)
        add_dependencies(camdroid_webcam_test camdroid_camera)
        add_test(NAME desktop_webcam COMMAND camdroid_webcam_test "$<TARGET_FILE:camdroid_camera>")
        set_tests_properties(desktop_webcam PROPERTIES TIMEOUT 20)
    endif()
    add_executable(camdroid_args_test tests/args_test.cpp)
    target_include_directories(camdroid_args_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/includes")
    target_sources(camdroid_args_test PRIVATE src/preview.cpp)
    target_compile_definitions(camdroid_args_test PRIVATE CAMDROID_GUI_TESTS)
    target_link_libraries(camdroid_args_test PRIVATE camdroid_webcam Qt6::Widgets Qt6::Network)
    add_test(NAME desktop_args COMMAND camdroid_args_test)
    set_tests_properties(desktop_args PROPERTIES TIMEOUT 30)
    add_test(NAME desktop_gui COMMAND camdroid_args_test --gui-tests)
    set_tests_properties(desktop_gui PROPERTIES TIMEOUT 15 ENVIRONMENT
        "QT_QPA_PLATFORM=minimal;QT_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/,>Qt6/plugins")
endif()
