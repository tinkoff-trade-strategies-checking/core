include(GoogleTest)

add_executable(core_tests
  version_test.cpp
  build/flags_test.cpp)
target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(core_tests PRIVATE core::simulation_core core_fixtures GTest::gtest_main rapidcheck_gtest core_warnings)
# Файлы с тестовыми данными (эталонный портфель): testdata/ в корне репозитория.
target_compile_definitions(core_tests PRIVATE
  CORE_TESTDATA_DIR="${PROJECT_SOURCE_DIR}/testdata"
  SIM_CLI_PATH="$<TARGET_FILE:sim_cli>")
add_dependencies(core_tests sim_cli)

if(CORE_BUILD_WORKER)
  add_executable(worker_tests worker_ping_test.cpp)
  target_include_directories(worker_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(worker_tests PRIVATE simulation_worker_lib core::simulation_core core_fixtures GTest::gtest_main core_warnings)
  target_compile_definitions(worker_tests PRIVATE
    CORE_TESTDATA_DIR="${PROJECT_SOURCE_DIR}/testdata"
    SIM_CLI_PATH="$<TARGET_FILE:sim_cli>")
  add_dependencies(worker_tests sim_cli)
  add_subdirectory(worker)
  add_subdirectory(clickhouse)
endif()

# Подкаталоги областей добавляют *_test.cpp к core_tests (и worker_tests). Сначала executables, потом области.
foreach(area money market portfolio exec engine report blind analysis cli)
  add_subdirectory(${area})
endforeach()

gtest_discover_tests(core_tests)
if(CORE_BUILD_WORKER)
  gtest_discover_tests(worker_tests)
endif()
