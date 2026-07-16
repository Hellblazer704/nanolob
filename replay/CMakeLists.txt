add_executable(nanolob_replay replay_binance.cpp)
target_link_libraries(nanolob_replay PRIVATE nanolob::nanolob nanolob_build_flags)
# Headers here are included as "replay/..." by the tool and by the tests.
target_include_directories(nanolob_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
