# Отчёт: метрики и расхождения. Исходники подхватываются автоматически.
file(GLOB report_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(report_sources)
  target_sources(simulation_core PRIVATE ${report_sources})
endif()
