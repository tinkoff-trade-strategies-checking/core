# Тесты области engine. Файлы *_test.cpp подхватываются автоматически.
file(GLOB engine_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(engine_tests)
  target_sources(core_tests PRIVATE ${engine_tests})
endif()
