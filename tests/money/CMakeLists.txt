# Тесты денег. core_tests - без protobuf; тест конвертации с настоящим proto - в worker_tests.
target_sources(core_tests PRIVATE decimal_test.cpp proto_conv_test.cpp)
if(CORE_BUILD_WORKER)
  target_sources(worker_tests PRIVATE proto_conv_generated_test.cpp)
endif()
