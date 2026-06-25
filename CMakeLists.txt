cmake_minimum_required(VERSION 3.14)
project(collab_media_controller LANGUAGES CXX)

# Use Foundation for logging. Default ON (matches upstream behaviour).
# Pass -DUSE_FOUNDATION=OFF to build without Foundation; logging falls back to std::cout/std::cerr.
option(USE_FOUNDATION "Use Foundation library for logging" ON)

# Platform detection message
if(ANDROID)
    message(STATUS "=== Building for ANDROID ===")
    message(STATUS "Android ABI: ${ANDROID_ABI}")
    message(STATUS "Android Platform: ${ANDROID_PLATFORM}")

    if(DEFINED ENV{GRPC_INSTALL_DIR})
        list(PREPEND CMAKE_PREFIX_PATH "$ENV{GRPC_INSTALL_DIR}")
        set(Protobuf_DIR "$ENV{GRPC_INSTALL_DIR}/lib/cmake/protobuf" CACHE PATH "Path to Android Protobuf CMake config")
        set(protobuf_DIR "${Protobuf_DIR}" CACHE PATH "Path to Android Protobuf CMake config (lowercase)" FORCE)
        set(gRPC_DIR "$ENV{GRPC_INSTALL_DIR}/lib/cmake/grpc" CACHE PATH "Path to Android gRPC CMake config")
        set(absl_DIR "$ENV{GRPC_INSTALL_DIR}/lib/cmake/absl" CACHE PATH "Path to Android absl CMake config")
        set(utf8_range_DIR "$ENV{GRPC_INSTALL_DIR}/lib/cmake/utf8_range" CACHE PATH "Path to Android utf8_range CMake config")
        set(re2_DIR "$ENV{GRPC_INSTALL_DIR}/lib/cmake/re2" CACHE PATH "Path to Android re2 CMake config")
        set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
        set(protobuf_MODULE_COMPATIBLE FALSE CACHE BOOL "Disable Protobuf Find-module compatibility" FORCE)
        message(STATUS "Using Android package prefix: $ENV{GRPC_INSTALL_DIR}")
        message(STATUS "Using Protobuf_DIR: ${Protobuf_DIR}")
        message(STATUS "Using gRPC_DIR: ${gRPC_DIR}")
    endif()

    if(DEFINED ENV{GRPC_HOST_INSTALL_DIR})
        set(HOST_PROTOC "$ENV{GRPC_HOST_INSTALL_DIR}/bin/protoc" CACHE FILEPATH "Host protoc executable")
        set(HOST_GRPC_CPP_PLUGIN "$ENV{GRPC_HOST_INSTALL_DIR}/bin/grpc_cpp_plugin" CACHE FILEPATH "Host grpc_cpp_plugin executable")
        message(STATUS "Using host protoc: ${HOST_PROTOC}")
        message(STATUS "Using host grpc_cpp_plugin: ${HOST_GRPC_CPP_PLUGIN}")
    endif()
else()
    message(STATUS "=== Building for LINUX ===")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find protobuf and gRPC once at the top level
if(ANDROID)
    find_package(protobuf CONFIG REQUIRED)
    # OpenSSL temporarily disabled - not needed for current Android build.
    # include(${CMAKE_CURRENT_LIST_DIR}/../android_openssl/android_openssl.cmake)
else()
    # For Linux, use CONFIG mode to ensure we use gRPC-provided protobuf cmake files
    find_package(protobuf CONFIG QUIET)
    if(NOT protobuf_FOUND)
        find_package(Protobuf REQUIRED)
    endif()
    # Find OpenSSL before gRPC (gRPC depends on it).
    #find_package(OpenSSL REQUIRED)
endif()

find_package(gRPC CONFIG REQUIRED)

if(USE_FOUNDATION)
    # Foundation: use the orchestrator's FindFoundation module which understands
    # both the source tree (Linux) and dist layouts (Android/Windows).
    set(ORCHESTRATOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../collab_orchestrator")
    list(APPEND CMAKE_MODULE_PATH "${ORCHESTRATOR_DIR}")
    find_package(Foundation REQUIRED)
endif()

add_subdirectory(sources)

if(NOT ANDROID)
    add_subdirectory(test)
endif()
