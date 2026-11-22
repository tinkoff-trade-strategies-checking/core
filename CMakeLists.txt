cmake_minimum_required(VERSION 3.25)

project(simulation_core
  VERSION 0.1.0
  DESCRIPTION "Simulation Core и Simulation Worker"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CORE_BUILD_WORKER "Собирать simulation_worker (нужны gRPC и protobuf)" ON)
option(CORE_BUILD_TESTS "Собирать тесты (нужен GoogleTest)" ON)

# Детерминизм (INV-4): результат расчёта не зависит от процессора и уровня оптимизации.
# -ffast-math, -Ofast и -funsafe-math-optimizations запрещены в любых флагах сборки.
foreach(flags_var CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                  CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
  if("${${flags_var}}" MATCHES "-ffast-math|-Ofast|-funsafe-math-optimizations|/fp:fast")
    message(FATAL_ERROR "${flags_var} содержит флаг быстрой математики: он нарушает детерминизм (INV-4)")
  endif()
endforeach()

# Предупреждения и флаги детерминизма применяются только к нашим целям, сгенерированный protobuf-код их не получает.
add_library(core_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # -ffp-contract=off: без слияния a * b + c в FMA (на x86-64 GCC делает это при -march с FMA).
  target_compile_options(core_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Werror -ffp-contract=off)
elseif(MSVC)
  target_compile_options(core_warnings INTERFACE /W4 /WX)
endif()

include(FetchContent)
set(DSL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DSL_SOURCE_DIR "" CACHE PATH "Локальный путь к репозиторию dsl для офлайн-сборки")
if(DSL_SOURCE_DIR)
  FetchContent_Declare(dsl SOURCE_DIR "${DSL_SOURCE_DIR}")
else()
  FetchContent_Declare(
    dsl
    GIT_REPOSITORY https://github.com/tinkoff-trade-strategies-checking/dsl.git
    GIT_TAG        cc0287f5ea002ee8dde1cf2e6532ec972ef52121
  )
endif()
FetchContent_MakeAvailable(dsl)

add_subdirectory(src/core)
add_subdirectory(src/fixtures)
add_subdirectory(src/cli)

if(CORE_BUILD_WORKER)
  find_package(Protobuf CONFIG QUIET)
  if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
  endif()
  find_package(gRPC CONFIG REQUIRED)
  add_subdirectory(proto)
  add_subdirectory(src/worker)
  target_link_libraries(simulation_worker_lib PRIVATE dsl::dsl)
endif()

if(CORE_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  find_package(rapidcheck CONFIG REQUIRED)
  add_subdirectory(tests)
  target_link_libraries(core_tests PRIVATE dsl::dsl)
  if(CORE_BUILD_WORKER)
    target_link_libraries(worker_tests PRIVATE dsl::dsl)
  endif()
endif()
