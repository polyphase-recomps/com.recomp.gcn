#!/bin/bash
# Native Addon Build Script for Linux/macOS
# Run this from the root of your addon folder (where package.json is)
#
# Usage: ./build.sh [config]
#   config - Optional. "Debug", "Release", or "Both" (default: Both)
#
# Requirements:
#   - g++ or clang++ installed

ADDON_NAME="com.recomp.gcn"
BUILD_CONFIG="${1:-Both}"

echo ""
echo "========================================"
echo " Building Native Addon: $ADDON_NAME"
echo " Configuration: $BUILD_CONFIG"
echo "========================================"
echo ""

if [ ! -d "Source" ]; then
    echo "ERROR: Source directory not found!"
    exit 1
fi

if command -v g++ &> /dev/null; then
    CXX="g++"
elif command -v clang++ &> /dev/null; then
    CXX="clang++"
else
    echo "ERROR: No C++ compiler found!"
    exit 1
fi

SOURCES=$(find Source -name "*.cpp" -type f)
BUILD_FAILED=0

if [ "$(uname -s)" = "Darwin" ]; then
    PLAT="Mac"; ARCH="$(uname -m)"; EXT="dylib"; PLATDEF="-DPLATFORM_MAC=1"; SHA="shasum -a 256"
    SHARED="-dynamiclib -Wl,-undefined,dynamic_lookup -arch $ARCH -mmacosx-version-min=12.0"
else
    PLAT="Linux"; ARCH="x64"; EXT="so"; PLATDEF="-DPLATFORM_LINUX=1"; SHA="sha256sum"
    SHARED="-shared"
fi

if [[ "$BUILD_CONFIG" == "Release" ]] || [[ "$BUILD_CONFIG" == "Both" ]]; then
    echo "Building Release configuration..."
    mkdir -p "build/$PLAT/$ARCH/Release"
    if $CXX $SHARED -fPIC -O2 -std=c++17 -ISource \
        -DOCTAVE_PLUGIN_EXPORT -DNDEBUG $PLATDEF \
        -o "build/$PLAT/$ARCH/Release/lib${ADDON_NAME}.$EXT" $SOURCES; then
        echo "Release build succeeded"
        $SHA "build/$PLAT/$ARCH/Release/lib${ADDON_NAME}.$EXT" > "build/$PLAT/$ARCH/Release/${ADDON_NAME}-$PLAT-$ARCH-Release.sha256"
    else
        echo "Release build FAILED!"
        BUILD_FAILED=1
    fi
fi

if [[ "$BUILD_CONFIG" == "Debug" ]] || [[ "$BUILD_CONFIG" == "Both" ]]; then
    echo "Building Debug configuration..."
    mkdir -p "build/$PLAT/$ARCH/Debug"
    if $CXX $SHARED -fPIC -O0 -g -std=c++17 -ISource \
        -DOCTAVE_PLUGIN_EXPORT -D_DEBUG $PLATDEF \
        -o "build/$PLAT/$ARCH/Debug/lib${ADDON_NAME}.$EXT" $SOURCES; then
        echo "Debug build succeeded"
        $SHA "build/$PLAT/$ARCH/Debug/lib${ADDON_NAME}.$EXT" > "build/$PLAT/$ARCH/Debug/${ADDON_NAME}-$PLAT-$ARCH-Debug.sha256"
    else
        echo "Debug build FAILED!"
        BUILD_FAILED=1
    fi
fi

echo ""
if [ $BUILD_FAILED -eq 1 ]; then
    echo "BUILD COMPLETED WITH ERRORS"
else
    echo "Build Succeeded!"
fi
echo "Output: build/Linux/x64/[Debug|Release]/lib${ADDON_NAME}.$EXT"

exit $BUILD_FAILED
