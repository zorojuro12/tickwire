cmake_minimum_required(VERSION 3.21)
project(tickwire CXX)

find_package(Threads REQUIRED)

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

add_library(libsim STATIC src/sim/sim.cpp src/sim/world.cpp)
target_include_directories(libsim PUBLIC src)
target_link_libraries(libsim PUBLIC tickwire_sim_flags)

add_library(libnet STATIC src/net/bytes.cpp src/net/protocol.cpp src/net/framing.cpp src/net/loopback.cpp src/net/udp.cpp src/net/snapshot_ring.cpp src/net/snapshot_delta.cpp)
target_link_libraries(libnet PUBLIC libsim)

add_library(libserver STATIC src/server/session.cpp src/server/clock.cpp src/server/tick_timer.cpp src/server/poll_set.cpp src/server/input_buffer.cpp src/server/rewind.cpp)
target_link_libraries(libserver PUBLIC libnet Threads::Threads)

add_library(libclient STATIC src/client/view.cpp src/client/clock_sync.cpp src/client/prediction.cpp src/client/interpolation.cpp)
target_include_directories(libclient PUBLIC src)
target_link_libraries(libclient PUBLIC libnet)

enable_testing()

function(tw_add_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE tests)
  target_link_libraries(${name} PRIVATE libserver libclient GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE libsim GTest::gtest_main)
add_test(NAME smoke_test COMMAND smoke_test)

add_executable(world_test tests/sim/world_test.cpp)
target_link_libraries(world_test PRIVATE libsim GTest::gtest_main)
add_test(NAME world_test COMMAND world_test)

tw_add_test(bytes_test tests/net/bytes_test.cpp)
tw_add_test(protocol_test tests/net/protocol_test.cpp)
tw_add_test(loopback_test tests/net/loopback_test.cpp)
tw_add_test(udp_test tests/net/udp_test.cpp)
tw_add_test(simulated_test tests/net/simulated_test.cpp)
tw_add_test(robustness_test tests/net/robustness_test.cpp)
tw_add_test(framing_test tests/net/framing_test.cpp)
tw_add_test(snapshot_ring_test tests/net/snapshot_ring_test.cpp)
tw_add_test(snapshot_delta_test tests/net/snapshot_delta_test.cpp)
tw_add_test(packet_ring_test tests/server/packet_ring_test.cpp)
tw_add_test(session_test tests/server/session_test.cpp)
tw_add_test(rewind_test tests/server/rewind_test.cpp)
tw_add_test(input_buffer_test tests/server/input_buffer_test.cpp)
tw_add_test(server_test tests/server/server_test.cpp)
tw_add_test(server_lagcomp_test tests/server/server_lagcomp_test.cpp)
tw_add_test(timer_test tests/server/timer_test.cpp)
tw_add_test(jitter_stats_test tests/server/jitter_stats_test.cpp)
tw_add_test(mutex_ring_test tests/server/mutex_ring_test.cpp)
tw_add_test(spsc_ring_test tests/server/spsc_ring_test.cpp)
tw_add_test(threaded_runner_test tests/server/threaded_runner_test.cpp)
tw_add_test(client_test tests/client/client_test.cpp)
tw_add_test(clock_sync_test tests/client/clock_sync_test.cpp)
tw_add_test(prediction_test tests/client/prediction_test.cpp)
tw_add_test(convergence_test tests/client/convergence_test.cpp)
tw_add_test(view_test tests/client/view_test.cpp)
tw_add_test(interpolation_test tests/client/interpolation_test.cpp)

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

  add_executable(fault_relaxed_ring tests/faults/relaxed_ring.cpp)
  target_link_libraries(fault_relaxed_ring PRIVATE tickwire_sim_flags Threads::Threads)
  add_test(NAME tsan_detects_relaxed_ring COMMAND fault_relaxed_ring)
  set_tests_properties(tsan_detects_relaxed_ring PROPERTIES
    PASS_REGULAR_EXPRESSION "data race")
endif()

add_executable(tw_server apps/tw_server.cpp)
target_link_libraries(tw_server PRIVATE libserver)
add_test(NAME server_app_runs COMMAND tw_server --port 0 --ticks 5)
set_tests_properties(server_app_runs PROPERTIES PASS_REGULAR_EXPRESSION "ticks=5")
add_test(NAME server_app_bogus_arg COMMAND tw_server --bogus)
set_tests_properties(server_app_bogus_arg PROPERTIES WILL_FAIL TRUE)
add_test(NAME server_app_threads1 COMMAND tw_server --port 0 --ticks 5 --threads 1)
set_tests_properties(server_app_threads1 PROPERTIES PASS_REGULAR_EXPRESSION "threads=1")
add_test(NAME server_app_threads2_spsc COMMAND tw_server --port 0 --ticks 5 --threads 2 --ring spsc)
set_tests_properties(server_app_threads2_spsc PROPERTIES PASS_REGULAR_EXPRESSION "threads=2 ring=spsc")
add_test(NAME server_app_threads2_mutex COMMAND tw_server --port 0 --ticks 5 --threads 2 --ring mutex)
set_tests_properties(server_app_threads2_mutex PROPERTIES PASS_REGULAR_EXPRESSION "threads=2 ring=mutex")
add_test(NAME server_app_batch_ingest
  COMMAND tw_server --port 0 --ticks 5 --threads 2 --ring spsc --batch-ingest)
set_tests_properties(server_app_batch_ingest PROPERTIES PASS_REGULAR_EXPRESSION "batch_ingest=1")
add_test(NAME server_app_sim_load
  COMMAND tw_server --port 0 --ticks 20 --threads 2 --ring spsc --sim-load-us 20000)
# CMake's built-in regex engine has no {n} bounded-repetition support, so the
# plan's `[0-9]{6}` is expanded here to six literal [0-9] terms with the same
# meaning: a p50 of >= 17'000'000 ns (8-9 digits, leading "17"-"19" or "2"-"9").
set_tests_properties(server_app_sim_load PROPERTIES
  PASS_REGULAR_EXPRESSION "jitter_ns p50=(1[7-9]|[2-9][0-9])[0-9][0-9][0-9][0-9][0-9][0-9]")

add_executable(tw_loadclient apps/tw_loadclient.cpp)
target_link_libraries(tw_loadclient PRIVATE libclient libserver)

add_test(NAME e2e_udp
  COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/e2e-udp.sh"
          "$<TARGET_FILE:tw_server>" "$<TARGET_FILE:tw_loadclient>")
# No PASS_REGULAR_EXPRESSION: CTest treats that property as the SOLE
# pass/fail signal once set, ignoring the process exit code entirely -- a
# script-level `exit 1` (e.g. the lead= check below) would be silently
# overridden by a regex match elsewhere in the output. Exit code alone
# (set -e/pipefail in the script, plus tw_loadclient's own
# joined==players return value) is the pass/fail signal here.

add_executable(bench_queue tools/bench_queue.cpp)
target_link_libraries(bench_queue PRIVATE libserver)
add_test(NAME bench_queue_spsc COMMAND bench_queue --ring spsc --items 10000 --rate 0)
set_tests_properties(bench_queue_spsc PROPERTIES PASS_REGULAR_EXPRESSION "ns_per_handoff_p50=[0-9]")
add_test(NAME bench_queue_mutex COMMAND bench_queue --ring mutex --items 10000 --rate 0)
set_tests_properties(bench_queue_mutex PROPERTIES PASS_REGULAR_EXPRESSION "ns_per_handoff_p50=[0-9]")

add_test(NAME bench_smoke
  COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/bench.sh" --smoke
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
# No PASS_REGULAR_EXPRESSION, per the same reasoning as e2e_udp: that
# property replaces the exit code as CTest's sole pass/fail signal once set,
# which would mask a set -e failure inside the script. Exit code alone is the
# signal here.

add_executable(digest_dump_a tools/digest_dump.cpp)
target_link_libraries(digest_dump_a PRIVATE libsim)

add_executable(digest_dump_b tools/digest_dump.cpp)
target_link_libraries(digest_dump_b PRIVATE libsim)

add_test(NAME determinism_two_binaries
  COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/determinism-test.sh"
          "$<TARGET_FILE:digest_dump_a>" "$<TARGET_FILE:digest_dump_b>"
          "${CMAKE_SOURCE_DIR}/scripts/compare-digests.sh")

option(TW_BUILD_GUI "Build the raylib client" OFF)
if(TW_BUILD_GUI)
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 4.5.0)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(raylib)

  # raylib's own warnings must not fail our build: -Wall -Wextra -Werror
  # come from tickwire_sim_flags, which raylib does not link, so applying
  # them only to our targets (not globally) keeps raylib's build silent.
  add_executable(tw_client apps/tw_client.cpp)
  target_link_libraries(tw_client PRIVATE libclient libserver raylib)
  add_test(NAME client_selftest COMMAND tw_client --selftest)
  set_tests_properties(client_selftest PROPERTIES SKIP_RETURN_CODE 77)
endif()
