# Тесты области blind. Файлы *_test.cpp подхватываются автоматически.
file(GLOB blind_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(blind_tests)
  target_sources(core_tests PRIVATE ${blind_tests})
endif()
