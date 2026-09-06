cmake_minimum_required(VERSION 3.21)
project(tickwire CXX)

include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG release-1.12.1)
FetchContent_MakeAvailable(googletest)

set(TW_SANITIZER "" CACHE STRING "address,undefined | thread | (empty)")

add_library(tickwire_sim_flags INTERFACE)
target_compile_features(tickwire_sim_flags INTERFACE cxx_std_20)
target_compile_options(tickwire_sim_flags INTERFACE
  -march=x86-64 -ffp-contract=off -Wall -Wextra -Werror)
if(TW_SANITIZER)
  target_compile_options(tickwire_sim_flags INTERFACE
    -fsanitize=${TW_SANITIZER} -g -fno-omit-frame-pointer)
  target_link_options(tickwire_sim_flags INTERFACE -fsanitize=${TW_SANITIZER})
endif()

add_library(libsim STATIC src/sim/sim.cpp)
target_include_directories(libsim PUBLIC src)
target_link_libraries(libsim PUBLIC tickwire_sim_flags)

add_library(libnet STATIC src/net/bytes.cpp src/net/protocol.cpp src/net/loopback.cpp src/net/udp.cpp)
target_link_libraries(libnet PUBLIC libsim)

enable_testing()

function(tw_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE libnet GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE libsim GTest::gtest_main)
add_test(NAME smoke_test COMMAND smoke_test)

tw_add_test(bytes_test tests/net/bytes_test.cpp)
tw_add_test(protocol_test tests/net/protocol_test.cpp)
tw_add_test(loopback_test tests/net/loopback_test.cpp)
tw_add_test(udp_test tests/net/udp_test.cpp)

if(TW_SANITIZER MATCHES "address")
  add_executable(fault_uaf tests/faults/uaf.cpp)
  target_link_libraries(fault_uaf PRIVATE tickwire_sim_flags)
  add_test(NAME asan_detects_uaf COMMAND fault_uaf)
  set_tests_properties(asan_detects_uaf PROPERTIES
    PASS_REGULAR_EXPRESSION "heap-use-after-free")

  add_executable(fault_ovf tests/faults/ovf.cpp)
  target_link_libraries(fault_ovf PRIVATE tickwire_sim_flags)
  add_test(NAME ubsan_detects_overflow COMMAND fault_ovf)
  set_tests_properties(ubsan_detects_overflow PROPERTIES
    PASS_REGULAR_EXPRESSION "signed integer overflow")
endif()

if(TW_SANITIZER MATCHES "thread")
  add_executable(fault_race tests/faults/race.cpp)
  target_link_libraries(fault_race PRIVATE tickwire_sim_flags)
  add_test(NAME tsan_detects_race COMMAND fault_race)
  set_tests_properties(tsan_detects_race PROPERTIES
    PASS_REGULAR_EXPRESSION "data race")
endif()

add_executable(digest_dump_a tools/digest_dump.cpp)
target_link_libraries(digest_dump_a PRIVATE libsim)

add_executable(digest_dump_b tools/digest_dump.cpp)
target_link_libraries(digest_dump_b PRIVATE libsim)

add_test(NAME determinism_two_binaries
  COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/determinism-test.sh"
          "$<TARGET_FILE:digest_dump_a>" "$<TARGET_FILE:digest_dump_b>"
          "${CMAKE_SOURCE_DIR}/scripts/compare-digests.sh")
