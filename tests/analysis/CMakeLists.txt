# Тесты области analysis. Файлы *_test.cpp подхватываются автоматически.
file(GLOB analysis_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(analysis_tests)
  target_sources(core_tests PRIVATE ${analysis_tests})
endif()
