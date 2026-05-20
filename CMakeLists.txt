cmake_minimum_required(VERSION 3.20)
project(cymon_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --------------------------------------------------------------------------
# Fetch GTest
# --------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

# --------------------------------------------------------------------------
# Stub include path (replaces ESP-IDF / FreeRTOS headers on Linux)
# --------------------------------------------------------------------------
set(STUB_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/tests/stubs)

# --------------------------------------------------------------------------
# Monitor engine library (platform-agnostic)
# --------------------------------------------------------------------------
add_library(cymon_monitor
  src/monitor/ring_buffer.cpp
  src/monitor/monitor_session.cpp
  src/monitor/session_manager.cpp
)
target_include_directories(cymon_monitor PUBLIC
  include
  ${STUB_INCLUDE_DIR}
)
target_compile_options(cymon_monitor PRIVATE -Wall -Wextra -Werror=return-type)

# --------------------------------------------------------------------------
# Test executable
# --------------------------------------------------------------------------
add_executable(cymon_tests
  tests/test_ring_buffer.cpp
  tests/test_monitor_session.cpp
  tests/test_node_record.cpp
)
target_link_libraries(cymon_tests
  cymon_monitor
  GTest::gtest_main
)
target_include_directories(cymon_tests PRIVATE
  include
  ${STUB_INCLUDE_DIR}
)
target_compile_options(cymon_tests PRIVATE -Wall -Wextra)

include(GoogleTest)
gtest_discover_tests(cymon_tests)

enable_testing()
