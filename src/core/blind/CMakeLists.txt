# Слепой режим: масштабирование цен, объёмов и сумм в % капитала.
file(GLOB blind_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(blind_sources)
  target_sources(simulation_core PRIVATE ${blind_sources})
endif()
