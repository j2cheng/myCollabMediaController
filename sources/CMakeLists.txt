# Path to MediaControllerInterface.h — defaults to mock/, override for mk2 app
set(MEDIACONTROLLER_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mock"
    CACHE PATH "Directory containing MediaControllerInterface.h")

# Proto file path
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/grpcstreamout/grpc_str_out.proto)
get_filename_component(PROTO_DIR ${PROTO_FILE} DIRECTORY)

set(PROTO_SRCS ${CMAKE_CURRENT_BINARY_DIR}/grpc_str_out.pb.cc)
set(PROTO_HDRS ${CMAKE_CURRENT_BINARY_DIR}/grpc_str_out.pb.h)
set(GRPC_SRCS ${CMAKE_CURRENT_BINARY_DIR}/grpc_str_out.grpc.pb.cc)
set(GRPC_HDRS ${CMAKE_CURRENT_BINARY_DIR}/grpc_str_out.grpc.pb.h)

if(ANDROID)
  if(NOT DEFINED HOST_PROTOC)
    message(FATAL_ERROR "HOST_PROTOC must be set for Android builds")
  endif()
  if(NOT DEFINED HOST_GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "HOST_GRPC_CPP_PLUGIN must be set for Android builds")
  endif()
  set(PROTOC_EXECUTABLE ${HOST_PROTOC})
  set(GRPC_CPP_PLUGIN_EXECUTABLE ${HOST_GRPC_CPP_PLUGIN})
else()
  set(PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)
  set(GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
endif()

# Generate .pb.cc / .pb.h from .proto (message/data classes)
add_custom_command(
  OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
  COMMAND ${PROTOC_EXECUTABLE}
  ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
       -I ${PROTO_DIR}
       ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
)

# Generate .grpc.pb.cc / .grpc.pb.h from .proto (service classes)
add_custom_command(
  OUTPUT ${GRPC_SRCS} ${GRPC_HDRS}
  COMMAND ${PROTOC_EXECUTABLE}
  ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
       --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
       -I ${PROTO_DIR}
       ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
)

add_library(mediacontroller STATIC
  MediaController.cpp
  grpcstreamout/StreamoutGrpcClient.cpp
  ${PROTO_SRCS}
  ${PROTO_HDRS}
  ${GRPC_SRCS}
  ${GRPC_HDRS}
)

target_include_directories(mediacontroller PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/grpcstreamout
  ${MEDIACONTROLLER_INTERFACE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(mediacontroller PUBLIC
  protobuf::libprotobuf
  gRPC::grpc++
)
