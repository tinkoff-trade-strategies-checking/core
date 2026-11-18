add_executable(sim_cli main.cpp replay.cpp json.cpp)
target_link_libraries(sim_cli PRIVATE core::simulation_core core_fixtures core_warnings)
