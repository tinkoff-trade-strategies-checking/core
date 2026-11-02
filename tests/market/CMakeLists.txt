# Тесты рыночных данных (без Docker и сервисов). Файлы *_test.cpp подхватываются автоматически.
file(GLOB market_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(market_tests)
  target_sources(core_tests PRIVATE ${market_tests})
endif()
