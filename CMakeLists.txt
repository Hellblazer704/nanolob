cmake_minimum_required(VERSION 3.23)
project(nanolob VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NANOLOB_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(NANOLOB_BUILD_BENCH "Build benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(NANOLOB_SANITIZE "Build with ASan + UBSan" OFF)
option(NANOLOB_COVERAGE "Build with gcov coverage instrumentation" OFF)
option(NANOLOB_TSAN "Build with ThreadSanitizer (for the lock-free ring)" OFF)

# The engine itself: header-only, no dependencies.
add_library(nanolob INTERFACE)
add_library(nanolob::nanolob ALIAS nanolob)
target_include_directories(nanolob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(nanolob INTERFACE cxx_std_20)

# Flags shared by all executables in this repo (tests, benches, tools).
add_library(nanolob_build_flags INTERFACE)
if(MSVC)
  target_compile_options(nanolob_build_flags INTERFACE /W4 /permissive-)
else()
  target_compile_options(nanolob_build_flags INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(NANOLOB_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "NANOLOB_SANITIZE is only wired up for GCC/Clang")
  endif()
  target_compile_options(nanolob_build_flags INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(nanolob_build_flags INTERFACE -fsanitize=address,undefined)
endif()

if(NANOLOB_COVERAGE)
  target_compile_options(nanolob_build_flags INTERFACE --coverage -O0 -g)
  target_link_options(nanolob_build_flags INTERFACE --coverage)
endif()

if(NANOLOB_TSAN)
  if(MSVC)
    message(FATAL_ERROR "NANOLOB_TSAN is only wired up for GCC/Clang")
  endif()
  # -O2 so the compiler is actually free to reorder around the atomics — a
  # TSan run at -O0 would miss orderings the optimizer is allowed to produce.
  target_compile_options(nanolob_build_flags INTERFACE
    -fsanitize=thread -fno-omit-frame-pointer -O2 -g)
  target_link_options(nanolob_build_flags INTERFACE -fsanitize=thread)
endif()

include(FetchContent)

if(NANOLOB_BUILD_TESTS)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(Catch2)
  enable_testing()
  add_subdirectory(tests)
endif()

if(NANOLOB_BUILD_BENCH AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt)
  add_subdirectory(bench)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/replay/CMakeLists.txt)
  add_subdirectory(replay)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/strategy/CMakeLists.txt)
  add_subdirectory(strategy)
endif()
