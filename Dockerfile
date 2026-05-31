# Self-contained build image for collab_media_controller (Android arm64-v8a).
#
# Mirrors the conventions in env.sh / build_grpc_host.sh / build_grpc.sh:
#   - GRPC v1.78.1, SSL disabled, static libs, module providers
#   - Host build produces protoc + grpc_cpp_plugin
#   - Target (Android arm64-v8a) build uses the cross-build-codegen-only patch
#
# Build:    docker build -t collab-media-controller-build .
# Use:      docker run --rm -v "$PWD":/work -w /work collab-media-controller-build \
#               bash -lc './build_android64.sh && ./dist_android64.sh'

FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

# ---- Versions (kept in sync with env.sh) -------------------------------------
ARG ANDROID_NDK_VERSION=30.0.14904198
ARG ANDROID_API_LEVEL=35
ARG ANDROID_BUILD_TOOLS_VERSION=36.0.0
ARG ANDROID_CMDLINE_TOOLS_VERSION=11076708
ARG GRPC_VERSION=v1.78.1
ARG CMAKE_VERSION=3.28.3

ENV ANDROID_HOME=/opt/android-sdk \
    ANDROID_SDK_ROOT=/opt/android-sdk \
    ANDROID_NDK_VERSION=${ANDROID_NDK_VERSION} \
    GRPC_VERSION=${GRPC_VERSION} \
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
        patch \
        python3 \
        openjdk-17-jdk-headless \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

# ---- Recent CMake (Ubuntu 22.04 ships 3.22; we want >=3.27) ------------------
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
    && sdkmanager --install \
        "platform-tools" \
        "platforms;android-${ANDROID_API_LEVEL}" \
        "build-tools;${ANDROID_BUILD_TOOLS_VERSION}" \
        "ndk;${ANDROID_NDK_VERSION}" > /dev/null \
    && test -d "${ANDROID_NDK_HOME}"

# ---- Clone gRPC source (single shared tree, used for host + target) ----------
RUN git clone --recurse-submodules --shallow-submodules \
        --depth 1 --branch ${GRPC_VERSION} \
        https://github.com/grpc/grpc.git /opt/grpc-src

# ---- Build gRPC for the host (provides protoc + grpc_cpp_plugin) -------------
# Flags mirror build_grpc_host.sh.
RUN cmake -S /opt/grpc-src -B /tmp/grpc-host-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17 \
        -DCMAKE_INSTALL_PREFIX=${GRPC_HOST_INSTALL_DIR} \
        -DgRPC_ABSL_PROVIDER=module \
        -DgRPC_BUILD_CODEGEN=ON \
        -DgRPC_BUILD_CSHARP_EXT=OFF \
        -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON \
        -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_REFLECTION=ON \
        -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
        -DgRPC_BUILD_SHARED_LIBS=OFF \
        -DgRPC_BUILD_STATIC_LIBS=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_CARES_PROVIDER=module \
        -DgRPC_INSTALL=ON \
        -DgRPC_PROTOBUF_PROVIDER=module \
        -DgRPC_RE2_PROVIDER=module \
        -DgRPC_SSL=OFF \
    && cmake --build /tmp/grpc-host-build -- -j"$(nproc)" \
    && cmake --install /tmp/grpc-host-build \
    && rm -rf /tmp/grpc-host-build

# ---- Build gRPC for Android arm64-v8a (target sysroot) -----------------------
# Flags mirror build_grpc.sh; the cross-build patch must be in the build context.
COPY scripts/patches/grpc-1.78.1-cross-build-codegen-only.patch /tmp/grpc-cross.patch
RUN patch -p1 -d /opt/grpc-src < /tmp/grpc-cross.patch \
    && cmake -S /opt/grpc-src -B /tmp/grpc-target-build -G Ninja \
        -DANDROID_ABI=${GRPC_ANDROID_ABI} \
        -DANDROID_PLATFORM=${GRPC_ANDROID_PLATFORM} \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17 \
        -DCMAKE_INSTALL_PREFIX=${GRPC_INSTALL_DIR} \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake \
        -D_gRPC_CPP_PLUGIN=${GRPC_HOST_INSTALL_DIR}/bin/grpc_cpp_plugin \
        -D_gRPC_PROTOBUF_PROTOC_EXECUTABLE=${GRPC_HOST_INSTALL_DIR}/bin/protoc \
        -DgRPC_ABSL_PROVIDER=module \
        -DgRPC_BUILD_CARES=OFF \
        -DgRPC_BUILD_CODEGEN=ON \
        -DgRPC_BUILD_CSHARP_EXT=OFF \
        -DgRPC_BUILD_GRPC_CPP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_CARES_PROVIDER=none \
        -DgRPC_INSTALL=ON \
        -DgRPC_PROTOBUF_PROVIDER=module \
        -DgRPC_RE2_PROVIDER=module \
        -DgRPC_SSL=OFF \
    && cmake --build /tmp/grpc-target-build -- -j"$(nproc)" \
    && cmake --install /tmp/grpc-target-build \
    && rm -rf /tmp/grpc-target-build /opt/grpc-src /tmp/grpc-cross.patch

# ---- env.sh shim — build_android64.sh sources $HOME/shared/platforms/android/env.sh
RUN mkdir -p /root/shared/platforms/android \
    && cat > /root/shared/platforms/android/env.sh <<EOF
# Generated by Dockerfile — pins NDK + gRPC paths for Android arm64-v8a build.
export ANDROID_HOME=${ANDROID_HOME}
export ANDROID_SDK_ROOT=${ANDROID_HOME}
export ANDROID_NDK_VERSION=${ANDROID_NDK_VERSION}
export ANDROID_NDK_HOME=${ANDROID_NDK_HOME}
export GRPC_VERSION=${GRPC_VERSION}
export GRPC_INSTALL_DIR=${GRPC_INSTALL_DIR}
export GRPC_HOST_INSTALL_DIR=${GRPC_HOST_INSTALL_DIR}
export GRPC_ANDROID_ABI=${GRPC_ANDROID_ABI}
export GRPC_ANDROID_PLATFORM=${GRPC_ANDROID_PLATFORM}
export PATH=/opt/cmake/bin:${ANDROID_HOME}/cmdline-tools/latest/bin:${ANDROID_HOME}/platform-tools:${ANDROID_NDK_HOME}:\${PATH}
EOF

WORKDIR /work
CMD ["bash"]
