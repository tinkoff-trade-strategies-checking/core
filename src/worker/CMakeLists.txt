# Сервис вынесен в библиотеку, чтобы тесты могли поднять его в процессе.
add_library(simulation_worker_lib
  worker_service.cpp)
target_include_directories(simulation_worker_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(simulation_worker_lib
  PUBLIC core::simulation_proto
  PRIVATE core::simulation_core core_warnings)

add_executable(simulation_worker main.cpp)
target_link_libraries(simulation_worker PRIVATE simulation_worker_lib core_warnings)
