# Адаптер чтения ClickHouse по HTTP (libcurl). Отдельная библиотека: не тянет gRPC.
find_package(CURL REQUIRED)

add_library(simulation_clickhouse
  clickhouse_client.cpp
  clickhouse_bar_source.cpp)
add_library(core::simulation_clickhouse ALIAS simulation_clickhouse)
target_include_directories(simulation_clickhouse PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(simulation_clickhouse
  PUBLIC core::simulation_core
  PRIVATE CURL::libcurl core_warnings)
