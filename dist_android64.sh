#!/bin/bash
set -e

echo "=== Creating distribution package ==="

# Define directories
DIST_DIR="collab_media_controller_dist"
DIST_INCLUDE="${DIST_DIR}/include"
DIST_CMAKE="${DIST_DIR}/lib/cmake"
DIST_LIB="${DIST_DIR}/android/arm64-v8a/lib/Release"

# 1. Create folder structure
echo "Creating directory structure..."
mkdir -p "${DIST_INCLUDE}"
mkdir -p "${DIST_CMAKE}"
mkdir -p "${DIST_LIB}"

# 2. Copy MediaControllerInterface.h
echo "Copying MediaControllerInterface.h..."
cp mock/MediaControllerInterface.h "${DIST_INCLUDE}/"

# 3. Copy generated makefiles to cmake folder
echo "Copying CMake files..."
if [ -d "build-arm64/sources/CMakeFiles" ]; then
    cp -r build-arm64/sources/CMakeFiles "${DIST_CMAKE}/" || true
fi
if [ -f "build-arm64/sources/cmake_install.cmake" ]; then
    cp build-arm64/sources/cmake_install.cmake "${DIST_CMAKE}/" || true
fi

# 4. Copy generated mediacontroller.a (64-bit)
echo "Copying mediacontroller.a..."
if [ -f "build-arm64/sources/libmediacontroller.a" ]; then
    cp build-arm64/sources/libmediacontroller.a "${DIST_LIB}/"
    echo "Copied libmediacontroller.a"
else
    echo "Warning: build-arm64/sources/libmediacontroller.a not found"
fi

# 6. Create zip archive
echo "Creating distribution zip file..."
rm -f collab_media_controller_dist.zip
zip -r collab_media_controller_dist.zip "${DIST_DIR}" > /dev/null

echo "=== Distribution package created successfully ==="
echo "Archive: collab_media_controller_dist.zip"
ls -lh collab_media_controller_dist.zip
