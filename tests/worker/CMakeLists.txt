# Тесты воркера (обработчики RPC, конвертеры). tests/worker_ping_test.cpp остаётся в tests/.
file(GLOB worker_area_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(worker_area_tests)
  target_sources(worker_tests PRIVATE ${worker_area_tests})
endif()
