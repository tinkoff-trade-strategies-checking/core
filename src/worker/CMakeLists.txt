# Сервис вынесен в библиотеку, чтобы тесты могли поднять его в процессе.
# Обработчики RPC (handlers/), конвертеры proto <-> ядро (convert/) и адаптер ClickHouse (clickhouse/)
# подхватываются автоматически; worker_service.cpp перечислен явно.
find_package(CURL REQUIRED)

file(GLOB worker_sources CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/handlers/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/convert/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/clickhouse/*.cpp)

add_library(simulation_worker_lib
  timeline_cache.cpp
  worker_service.cpp
  ${worker_sources})
target_include_directories(simulation_worker_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(simulation_worker_lib
  PUBLIC core::simulation_proto core::simulation_core
  PRIVATE CURL::libcurl core_warnings)

add_executable(simulation_worker main.cpp)
target_link_libraries(simulation_worker PRIVATE simulation_worker_lib core_warnings)
