cmake_minimum_required(VERSION 3.10)
project(hello_grpc_client CXX)

set(CMAKE_CXX_STANDARD 17)

set(Protobuf_INCLUDE_DIR "C:/Users/JSuppe/cmake/include")
set(Protobuf_LIBRARIES "C:/Users/JSuppe/cmake/lib/protobuf") 

# Find Protobuf and gRPC packages
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)


# Proto file
set(PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/protos/packagehello.proto)


# Generate protobuf and gRPC sources using custom commands
set(PROTO_HDRS ${CMAKE_CURRENT_SOURCE_DIR}/protos/packagehello.pb.h ${CMAKE_CURRENT_SOURCE_DIR}/protos/packagehello.grpc.pb.h)
set(PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/protos/packagehello.pb.cc ${CMAKE_CURRENT_SOURCE_DIR}/protos/packagehello.grpc.pb.cc)

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND protobuf::protoc
        ARGS --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/protos
             --cpp_out=${CMAKE_CURRENT_SOURCE_DIR}/protos
             --grpc_out=${CMAKE_CURRENT_SOURCE_DIR}/protos
             --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
             ${CMAKE_CURRENT_SOURCE_DIR}/protos/packagehello.proto
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protos/packagehello.proto
    COMMENT "Generating C++ protocol buffer and gRPC sources for packagehello.proto"
)

include_directories(
    ${Protobuf_INCLUDE_DIRS}
    ${gRPC_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/protos
)


add_executable(client
    grpc/client.cpp
    ${PROTO_SRCS}
)

target_link_libraries(client
    ${Protobuf_LIBRARIES}
    ${gRPC_LIBRARIES}
    gRPC::grpc++
)