# Портфель и сделки. Исходники подхватываются автоматически.
file(GLOB portfolio_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(portfolio_sources)
  target_sources(simulation_core PRIVATE ${portfolio_sources})
endif()
