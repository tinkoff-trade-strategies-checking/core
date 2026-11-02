# Тесты области portfolio. Файлы *_test.cpp подхватываются автоматически.
file(GLOB portfolio_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(portfolio_tests)
  target_sources(core_tests PRIVATE ${portfolio_tests})
endif()
