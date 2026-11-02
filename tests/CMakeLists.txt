include(GoogleTest)

add_executable(core_tests
  determinism_test.cpp
  version_test.cpp)
target_link_libraries(core_tests PRIVATE core::simulation_core GTest::gtest_main core_warnings)
gtest_discover_tests(core_tests)

if(CORE_BUILD_WORKER)
  add_executable(worker_tests worker_ping_test.cpp)
  target_link_libraries(worker_tests PRIVATE simulation_worker_lib core::simulation_core GTest::gtest_main core_warnings)
  add_subdirectory(clickhouse)
  gtest_discover_tests(worker_tests)
endif()

add_subdirectory(money)

add_subdirectory(market)
