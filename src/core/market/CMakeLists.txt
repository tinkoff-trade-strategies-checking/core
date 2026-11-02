# Рыночные данные: Bar, BarSource, InMemoryBarSource (ФТ-3, ФТ-5, INV-1). Исходники подхватываются автоматически.
file(GLOB market_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(market_sources)
  target_sources(simulation_core PRIVATE ${market_sources})
endif()
