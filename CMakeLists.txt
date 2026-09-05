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

enable_testing()
add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE libsim GTest::gtest_main)
add_test(NAME smoke_test COMMAND smoke_test)
