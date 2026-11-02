# Язык стратегий: лексер, парсер, типы, чтение будущего (INV-1). Исходники подхватываются автоматически.
file(GLOB dsl_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(dsl_sources)
  target_sources(simulation_core PRIVATE ${dsl_sources})
endif()
