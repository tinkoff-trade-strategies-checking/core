# Тесты области exec. Файлы *_test.cpp подхватываются автоматически.
file(GLOB exec_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(exec_tests)
  target_sources(core_tests PRIVATE ${exec_tests})
endif()
