# Анализ рынка: классификатор категории исторического периода.
file(GLOB analysis_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(analysis_sources)
  target_sources(simulation_core PRIVATE ${analysis_sources})
endif()
