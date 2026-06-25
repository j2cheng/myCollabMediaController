#!/bin/bash
set -e

# Remove previous 64-bit android build directory
rm -rf build-arm64

# Source Android environment variables
. "$HOME/shared/platforms/android/env.sh"

# Configure CMake for 64-bit Android build
cmake -S . -B build-arm64 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DUSE_FOUNDATION=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="arm64-v8a" \
  -DANDROID_PLATFORM="$GRPC_ANDROID_PLATFORM" \
  -DProtobuf_DIR="$GRPC_INSTALL_DIR/lib/cmake/protobuf" \
  -DgRPC_DIR="$GRPC_INSTALL_DIR/lib/cmake/grpc" \
  -DHOST_PROTOC="$GRPC_HOST_INSTALL_DIR/bin/protoc" \
  -DHOST_GRPC_CPP_PLUGIN="$GRPC_HOST_INSTALL_DIR/bin/grpc_cpp_plugin"

# Build the project
cmake --build build-arm64
