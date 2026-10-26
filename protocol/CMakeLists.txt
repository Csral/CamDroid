cmake_minimum_required(VERSION 3.20)
project(CamDroidProtocol VERSION 1.0 LANGUAGES CXX)

add_library(camdroid_protocol STATIC
    src/serializer.cpp
    src/parser.cpp
    src/session.cpp
    src/packet_builder.cpp
    src/video_payload.cpp
    src/utils.cpp
)

target_compile_features(camdroid_protocol
    PUBLIC cxx_std_20
)

target_include_directories(camdroid_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/includes
)

enable_testing()

add_executable(protocol_test
    tests/main.cpp
)

target_link_libraries(protocol_test PRIVATE
    camdroid_protocol
)

add_test(NAME protocol_roundtrip COMMAND protocol_test)

add_executable(protocol_session_test tests/session_test.cpp)
target_link_libraries(protocol_session_test PRIVATE camdroid_protocol)
add_test(NAME protocol_session COMMAND protocol_session_test)

add_executable(protocol_video_test tests/video_test.cpp)
target_link_libraries(protocol_video_test PRIVATE camdroid_protocol)
add_test(NAME protocol_video COMMAND protocol_video_test)
