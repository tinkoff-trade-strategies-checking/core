# Тесты адаптера ClickHouse: юнит-тесты с подменой клиента и интеграционный (нужен CLICKHOUSE_URL).
target_sources(worker_tests PRIVATE clickhouse_bar_source_test.cpp clickhouse_integration_test.cpp)
