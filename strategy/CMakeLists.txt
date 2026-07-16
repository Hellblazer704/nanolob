add_executable(nanolob_mmsim mm_sim.cpp)
target_link_libraries(nanolob_mmsim PRIVATE nanolob::nanolob nanolob_build_flags)
target_include_directories(nanolob_mmsim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
