include(FetchContent)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# benchmark v1.9.1 doesn't compile warning-clean under GCC 16; its -Werror is
# its problem, not ours.
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(benchmark)

# Main benchmark binary: engine design points + SPSC ring.
add_executable(nanolob_bench bench_main.cpp bench_engine.cpp bench_ring.cpp)
target_link_libraries(nanolob_bench
  PRIVATE nanolob::nanolob nanolob_build_flags benchmark::benchmark)
target_include_directories(nanolob_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Same engine benchmarks with cache-line padding stripped from book nodes,
# to measure what the alignment buys (see BENCHMARKS.md).
add_executable(nanolob_bench_unaligned bench_main.cpp bench_engine.cpp)
target_link_libraries(nanolob_bench_unaligned
  PRIVATE nanolob::nanolob nanolob_build_flags benchmark::benchmark)
target_include_directories(nanolob_bench_unaligned PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nanolob_bench_unaligned
  PRIVATE NANOLOB_NO_CACHE_ALIGN NANOLOB_BENCH_TAG="unaligned:")
