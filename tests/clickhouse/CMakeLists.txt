# Тесты адаптера ClickHouse: юнит-тесты с подменой клиента и интеграционный (нужен CLICKHOUSE_URL).
file(GLOB clickhouse_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
target_sources(worker_tests PRIVATE ${clickhouse_tests})
