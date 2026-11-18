add_library(core_fixtures STATIC support/fixture1.cpp)
target_include_directories(core_fixtures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(core_fixtures PUBLIC core::simulation_core PRIVATE core_warnings)
