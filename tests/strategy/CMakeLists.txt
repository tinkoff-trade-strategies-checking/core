# Тесты области strategy. Файлы *_test.cpp подхватываются автоматически.
file(GLOB strategy_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(strategy_tests)
  target_sources(core_tests PRIVATE ${strategy_tests})
endif()
