# Десятичный тип для денег (ADR-0008, INV-6). Исходники подхватываются автоматически.
file(GLOB money_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(money_sources)
  target_sources(simulation_core PRIVATE ${money_sources})
endif()
