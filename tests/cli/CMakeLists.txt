# Тесты CLI sim_cli. Файлы *_test.cpp подхватываются автоматически.
file(GLOB cli_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(cli_tests)
  target_sources(core_tests PRIVATE ${cli_tests})
endif()
