# Модель исполнения заявок (INV-2, INV-3). Исходники подхватываются автоматически.
file(GLOB exec_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(exec_sources)
  target_sources(simulation_core PRIVATE ${exec_sources})
endif()
