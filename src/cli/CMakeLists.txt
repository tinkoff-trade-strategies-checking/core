add_executable(sim_cli main.cpp)
target_link_libraries(sim_cli PRIVATE core::simulation_core core_warnings)
