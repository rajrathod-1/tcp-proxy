include(GoogleTest)

add_executable(proxy_tests
    test_balancer.cpp
    test_buffer.cpp
    test_config.cpp
    test_telemetry.cpp
    test_proxy.cpp)

target_link_libraries(proxy_tests PRIVATE proxy_core GTest::gtest_main)
proxy_set_warnings(proxy_tests)

gtest_discover_tests(proxy_tests DISCOVERY_TIMEOUT 30)
target_compile_definitions(proxy_tests PRIVATE PROXY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
