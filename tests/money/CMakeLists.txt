# Тесты денег. core_tests - без protobuf; тест конвертации с настоящим proto (*_generated_test.cpp) - в worker_tests.
file(GLOB money_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
set(money_generated_tests ${money_tests})
list(FILTER money_tests EXCLUDE REGEX "_generated_test\.cpp$")
list(FILTER money_generated_tests INCLUDE REGEX "_generated_test\.cpp$")
target_sources(core_tests PRIVATE ${money_tests})
if(CORE_BUILD_WORKER AND money_generated_tests)
  target_sources(worker_tests PRIVATE ${money_generated_tests})
endif()
