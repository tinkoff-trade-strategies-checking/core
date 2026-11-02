# Тесты области dsl. Файлы *_test.cpp подхватываются автоматически.
file(GLOB dsl_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(dsl_tests)
  target_sources(core_tests PRIVATE ${dsl_tests})
endif()
