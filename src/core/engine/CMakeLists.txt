# Рыночный цикл: шкала времени, шаги, журнал команд. Исходники подхватываются автоматически.
file(GLOB engine_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(engine_sources)
  target_sources(simulation_core PRIVATE ${engine_sources})
endif()
