cmake_minimum_required(VERSION 3.20)

project(CamDroidAndroid LANGUAGES CXX)

add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../protocol"
        "${CMAKE_CURRENT_BINARY_DIR}/protocol"
)

add_library(camdroid_native SHARED
        native_bridge.cpp
        native_transport.cpp
        transport_bridge.cpp
)

target_link_libraries(camdroid_native PRIVATE
        camdroid_protocol
        android
)
