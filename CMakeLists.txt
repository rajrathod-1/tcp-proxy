# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)

project(tcp_proxy
    VERSION 1.0.0
    DESCRIPTION "Multithreaded Layer 4 TCP proxy and load balancer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(PROXY_BUILD_TESTS        "Build the test suite"              ON)
option(PROXY_ENABLE_SANITIZERS  "Build with address+UB sanitizers"  OFF)

include(cmake/CompilerWarnings.cmake)
include(cmake/Dependencies.cmake)

find_package(Threads REQUIRED)

# Everything except main(): the tests drive the same library in-process.
add_library(proxy_core
    src/config.cpp
    src/net.cpp
    src/poller.cpp
    src/proxy.cpp
    src/telemetry.cpp)
target_include_directories(proxy_core PUBLIC include)
target_link_libraries(proxy_core PUBLIC Threads::Threads)
proxy_set_warnings(proxy_core)

add_executable(tcp-proxy src/main.cpp)
target_link_libraries(tcp-proxy PRIVATE proxy_core)
proxy_set_warnings(tcp-proxy)

# The load-testing kit: a backend and a client.
add_executable(echo-server tools/echo_server.cpp)
target_link_libraries(echo-server PRIVATE proxy_core)
proxy_set_warnings(echo-server)

add_executable(loadgen tools/loadgen.cpp)
target_link_libraries(loadgen PRIVATE proxy_core)
proxy_set_warnings(loadgen)

if(PROXY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
