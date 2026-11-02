# Чистый C++20, без gRPC и protobuf: ядро не знает про транспорт.
add_library(simulation_core
  version.cpp
  deterministic_run.cpp)
add_library(core::simulation_core ALIAS simulation_core)

target_include_directories(simulation_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(simulation_core PUBLIC cxx_std_20)
target_link_libraries(simulation_core PRIVATE core_warnings)

add_subdirectory(money)

add_subdirectory(market)
