cmake_minimum_required(VERSION 3.14)
project(collab_media_controller LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find protobuf and gRPC once at the top level
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

add_subdirectory(sources)
add_subdirectory(test)
