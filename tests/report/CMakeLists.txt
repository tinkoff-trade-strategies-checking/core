# Тесты области report. Файлы *_test.cpp подхватываются автоматически.
file(GLOB report_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(report_tests)
  target_sources(core_tests PRIVATE ${report_tests})
endif()
