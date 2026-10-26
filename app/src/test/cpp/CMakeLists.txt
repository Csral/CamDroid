cmake_minimum_required(VERSION 3.20)
project(CamDroidAndroidTransportTests LANGUAGES CXX)

# Host POSIX loopback harness. It builds only into the chosen test build folder;
# no protocol or desktop source/build configuration is modified.
find_package(Threads REQUIRED)
set(PROTOCOL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../protocol")
set(ANDROID_CPP "${CMAKE_CURRENT_SOURCE_DIR}/../../main/cpp")

add_executable(native_transport_test
    native_transport_test.cpp
    "${ANDROID_CPP}/native_transport.cpp"
    "${PROTOCOL_ROOT}/src/session.cpp"
    "${PROTOCOL_ROOT}/src/parser.cpp"
    "${PROTOCOL_ROOT}/src/serializer.cpp"
    "${PROTOCOL_ROOT}/src/packet_builder.cpp"
    "${PROTOCOL_ROOT}/src/video_payload.cpp"
    "${PROTOCOL_ROOT}/src/utils.cpp"
)
target_compile_features(native_transport_test PRIVATE cxx_std_20)
target_include_directories(native_transport_test PRIVATE "${ANDROID_CPP}" "${PROTOCOL_ROOT}/includes")
target_link_libraries(native_transport_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME native_transport COMMAND native_transport_test)
set_tests_properties(native_transport PROPERTIES TIMEOUT 30)
