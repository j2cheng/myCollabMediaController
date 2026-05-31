# Self-contained build image for collab_media_controller (Android arm64-v8a).
#
# Provides:
#   - build toolchain (cmake, ninja, g++, git, python3, zip, unzip, curl, openjdk)
#   - Android SDK cmdline-tools and NDK 30.0.14904198
#   - gRPC built twice:
#       * host install at $GRPC_HOST_INSTALL_DIR (provides protoc / grpc_cpp_plugin)
#       * Android arm64-v8a install at $GRPC_INSTALL_DIR (sysroot for target build)
#   - $HOME/shared/platforms/android/env.sh so build_android64.sh works unchanged
#
# Build:    docker build -t collab-media-controller-build .
# Use:      docker run --rm -v "$PWD":/work -w /work collab-media-controller-build \
#               bash -c "./build_android64.sh && ./dist_android64.sh"

FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

# ---- Versions -----------------------------------------------------------------
ARG ANDROID_NDK_VERSION=30.0.14904198
ARG ANDROID_API_LEVEL=24
ARG ANDROID_CMDLINE_TOOLS_VERSION=11076708
ARG GRPC_VERSION=v1.78.1
ARG CMAKE_VERSION=3.28.3

ENV ANDROID_HOME=/opt/android-sdk \
    ANDROID_SDK_ROOT=/opt/android-sdk \
    ANDROID_NDK_VERSION=${ANDROID_NDK_VERSION} \
    GRPC_INSTALL_DIR=/opt/grpc-android-arm64 \
    GRPC_HOST_INSTALL_DIR=/opt/grpc-host \
    GRPC_ANDROID_ABI=arm64-v8a \
    GRPC_ANDROID_PLATFORM=android-${ANDROID_API_LEVEL} \
    PATH=/opt/cmake/bin:/opt/android-sdk/cmdline-tools/latest/bin:/opt/android-sdk/platform-tools:${PATH}

ENV ANDROID_NDK_HOME=${ANDROID_HOME}/ndk/${ANDROID_NDK_VERSION}

# ---- Base packages ------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        autoconf \
        automake \
        libtool \
        pkg-config \
        git \
        curl \
        ca-certificates \
        unzip \
        zip \
        python3 \
        openjdk-17-jdk-headless \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

# ---- Recent CMake (Ubuntu 22.04 ships 3.22; we want >=3.27 for NDK r27/30) ----
RUN curl -fsSL https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz \
      | tar -xz -C /opt \
    && mv /opt/cmake-${CMAKE_VERSION}-linux-x86_64 /opt/cmake \
    && cmake --version

# ---- Android SDK cmdline-tools + NDK -----------------------------------------
RUN mkdir -p ${ANDROID_HOME}/cmdline-tools \
    && curl -fsSL -o /tmp/cmdline-tools.zip \
        https://dl.google.com/android/repository/commandlinetools-linux-${ANDROID_CMDLINE_TOOLS_VERSION}_latest.zip \
    && unzip -q /tmp/cmdline-tools.zip -d ${ANDROID_HOME}/cmdline-tools \
    && mv ${ANDROID_HOME}/cmdline-tools/cmdline-tools ${ANDROID_HOME}/cmdline-tools/latest \
    && rm /tmp/cmdline-tools.zip \
    && yes | sdkmanager --licenses > /dev/null \
    && sdkmanager --install "platform-tools" "ndk;${ANDROID_NDK_VERSION}" > /dev/null \
    && test -d "${ANDROID_NDK_HOME}"

# ---- Build gRPC for the host (provides protoc + grpc_cpp_plugin) -------------
RUN git clone --depth 1 --branch ${GRPC_VERSION} https://github.com/grpc/grpc.git /tmp/grpc \
    && cd /tmp/grpc \
    && git submodule update --init --depth 1 --recursive \
    && cmake -S . -B build-host -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${GRPC_HOST_INSTALL_DIR} \
        -DgRPC_INSTALL=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_SSL_PROVIDER=package \
        -DgRPC_ZLIB_PROVIDER=module \
        -DgRPC_CARES_PROVIDER=module \
        -DgRPC_RE2_PROVIDER=module \
        -DgRPC_PROTOBUF_PROVIDER=module \
        -DgRPC_ABSL_PROVIDER=module \
    && cmake --build build-host --target install \
    && rm -rf build-host

# ---- Build gRPC for Android arm64-v8a (sysroot for the target build) ---------
RUN cd /tmp/grpc \
    && cmake -S . -B build-android -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake \
        -DANDROID_ABI=${GRPC_ANDROID_ABI} \
        -DANDROID_PLATFORM=${GRPC_ANDROID_PLATFORM} \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${GRPC_INSTALL_DIR} \
        -DgRPC_INSTALL=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_BUILD_CODEGEN=OFF \
        -DgRPC_BUILD_GRPC_CPP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
        -DgRPC_SSL_PROVIDER=module \
        -DgRPC_ZLIB_PROVIDER=module \
        -DgRPC_CARES_PROVIDER=module \
        -DgRPC_RE2_PROVIDER=module \
        -DgRPC_PROTOBUF_PROVIDER=module \
        -DgRPC_ABSL_PROVIDER=module \
    && cmake --build build-android --target install \
    && rm -rf /tmp/grpc

# ---- env.sh shim — build_android64.sh sources $HOME/shared/platforms/android/env.sh
RUN mkdir -p /root/shared/platforms/android \
    && cat > /root/shared/platforms/android/env.sh <<'EOF'
# Generated by Dockerfile — pins NDK + gRPC paths for Android arm64-v8a build.
export ANDROID_HOME=/opt/android-sdk
export ANDROID_SDK_ROOT=/opt/android-sdk
export ANDROID_NDK_VERSION=30.0.14904198
export ANDROID_NDK_HOME="${ANDROID_HOME}/ndk/${ANDROID_NDK_VERSION}"
export GRPC_INSTALL_DIR=/opt/grpc-android-arm64
export GRPC_HOST_INSTALL_DIR=/opt/grpc-host
export GRPC_ANDROID_ABI=arm64-v8a
export GRPC_ANDROID_PLATFORM=android-24
export PATH="/opt/cmake/bin:${ANDROID_HOME}/cmdline-tools/latest/bin:${ANDROID_HOME}/platform-tools:${PATH}"
EOF

WORKDIR /work
CMD ["bash"]
