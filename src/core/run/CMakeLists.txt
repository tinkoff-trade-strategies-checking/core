# Прогоны: стратегия по данным, walk-forward. Исходники подхватываются автоматически.
file(GLOB run_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(run_sources)
  target_sources(simulation_core PRIVATE ${run_sources})
endif()
