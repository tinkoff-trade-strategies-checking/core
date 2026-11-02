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

# Предупреждения применяются только к нашим целям, сгенерированный protobuf-код их не получает.
add_library(core_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(core_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
  target_compile_options(core_warnings INTERFACE /W4 /WX)
endif()

add_subdirectory(src/core)
add_subdirectory(src/cli)

if(CORE_BUILD_WORKER)
  find_package(Protobuf CONFIG QUIET)
  if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
  endif()
  find_package(gRPC CONFIG REQUIRED)
  add_subdirectory(proto)
  add_subdirectory(src/worker)
endif()

if(CORE_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_subdirectory(tests)
endif()
