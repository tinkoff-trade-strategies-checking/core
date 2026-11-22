# Исполнение стратегий DSL и рантайм-детекторы ФТ-14. Исходники подхватываются автоматически.
file(GLOB strategy_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(strategy_sources)
  target_sources(simulation_core PRIVATE ${strategy_sources})
endif()
