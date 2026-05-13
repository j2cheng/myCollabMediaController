# Path to MediaControllerInterface.h — defaults to mock/, override for mk2 app
set(MEDIACONTROLLER_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mock"
    CACHE PATH "Directory containing MediaControllerInterface.h")

# Proto file path
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/grpcstreamout/grpc_str_out.proto)

# Generate .pb.cc / .pb.h from .proto (message/data classes)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILE})

# Generate .grpc.pb.cc / .grpc.pb.h from .proto (service classes)
get_filename_component(PROTO_DIR ${PROTO_FILE} DIRECTORY)
set(GRPC_SRCS ${CMAKE_CURRENT_BINARY_DIR}/grpc_str_out.grpc.pb.cc)
set(GRPC_HDRS ${CMAKE_CURRENT_BINARY_DIR}/grpc_str_out.grpc.pb.h)
add_custom_command(
  OUTPUT ${GRPC_SRCS} ${GRPC_HDRS}
  COMMAND protobuf::protoc
  ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
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
