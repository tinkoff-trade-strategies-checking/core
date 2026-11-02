# Тесты области run. Файлы *_test.cpp подхватываются автоматически.
file(GLOB run_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(run_tests)
  target_sources(core_tests PRIVATE ${run_tests})
endif()
