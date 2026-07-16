add_executable(nanolob_tests
  test_pool.cpp
  test_flat_hash_map.cpp
  test_engine.cpp
  test_differential.cpp
  test_spsc_ring.cpp
)
target_link_libraries(nanolob_tests
  PRIVATE nanolob::nanolob nanolob_build_flags Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(nanolob_tests)
