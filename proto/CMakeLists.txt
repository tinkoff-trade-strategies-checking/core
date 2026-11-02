# Генерация C++ кода из .proto. Вручную через protoc, чтобы работало одинаково
# с apt-пакетами и с vcpkg.
if(TARGET protobuf::protoc)
  set(_protoc $<TARGET_FILE:protobuf::protoc>)
else()
  set(_protoc ${Protobuf_PROTOC_EXECUTABLE})
endif()
if(TARGET gRPC::grpc_cpp_plugin)
  set(_grpc_plugin $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
else()
  find_program(_grpc_plugin grpc_cpp_plugin REQUIRED)
endif()

set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${_gen_dir})

set(_proto_files simulation common)
set(_gen_sources)
foreach(_name IN LISTS _proto_files)
  set(_out
    ${_gen_dir}/${_name}.pb.cc ${_gen_dir}/${_name}.pb.h
    ${_gen_dir}/${_name}.grpc.pb.cc ${_gen_dir}/${_name}.grpc.pb.h)
  add_custom_command(
    OUTPUT ${_out}
    COMMAND ${_protoc}
      --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
      --cpp_out=${_gen_dir}
      --grpc_out=${_gen_dir}
      --plugin=protoc-gen-grpc=${_grpc_plugin}
      ${CMAKE_CURRENT_SOURCE_DIR}/${_name}.proto
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_name}.proto
    COMMENT "protoc ${_name}.proto"
    VERBATIM)
  list(APPEND _gen_sources ${_gen_dir}/${_name}.pb.cc ${_gen_dir}/${_name}.grpc.pb.cc)
endforeach()

add_library(simulation_proto STATIC ${_gen_sources})
add_library(core::simulation_proto ALIAS simulation_proto)
target_include_directories(simulation_proto SYSTEM PUBLIC ${_gen_dir})
target_link_libraries(simulation_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)
target_compile_features(simulation_proto PUBLIC cxx_std_20)
