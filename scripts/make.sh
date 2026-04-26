#!/usr/bin/env bash
# ---------------------------------------------------------------------------------------
# make.sh — configure + build + (optionally) test libmem
#
# Usage:
#   ./scripts/make.sh                    # Default: Debug, Clang via LLVM toolchain
#   ./scripts/make.sh --release          # Release build
#   ./scripts/make.sh --gcc              # Use GCC instead of Clang
#   ./scripts/make.sh --test             # Build and run tests
#   ./scripts/make.sh --shared           # Build as shared library
#   ./scripts/make.sh --clean            # Remove build directory first
#   ./scripts/make.sh --release --test   # Combine flags as needed
# ---------------------------------------------------------------------------------------

set -euo pipefail

# ---------------------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------------------
BUILD_TYPE="Debug"
USE_GCC=false
BUILD_TESTS=false
BUILD_SHARED=false
CLEAN=false

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SOURCE_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${SOURCE_DIR}/build"

# ---------------------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------------------
for arg in "$@"; do
    case "${arg}" in
        --release)  BUILD_TYPE="Release" ;;
        --gcc)      USE_GCC=true ;;
        --test)     BUILD_TESTS=true ;;
        --shared)   BUILD_SHARED=true ;;
        --clean)    CLEAN=true ;;
        *)
            echo "Unknown option: ${arg}"
            echo "Usage: $0 [--release] [--gcc] [--test] [--shared] [--clean]"
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------------------
if ${CLEAN}; then
    echo "==> Cleaning build directory"
    rm -rf "${BUILD_DIR}"
fi

# ---------------------------------------------------------------------------------------
# CMake arguments
# ---------------------------------------------------------------------------------------
CMAKE_ARGS=(
    -S "${SOURCE_DIR}"
    -B "${BUILD_DIR}"
    -G Ninja
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
)

if ${BUILD_TESTS}; then
    CMAKE_ARGS+=("-DBUILD_TESTS=ON")
fi

if ${BUILD_SHARED}; then
    CMAKE_ARGS+=("-DBUILD_SHARED_LIBS=ON")
fi

# ---------------------------------------------------------------------------------------
# Compiler selection
# ---------------------------------------------------------------------------------------
if ${USE_GCC}; then
    echo "==> Using GCC"
    # Let CMake find gcc/g++ from PATH (or override with CC/CXX env vars)
    if [ -z "${CC:-}" ]; then
        export CC="gcc"
    fi
    if [ -z "${CXX:-}" ]; then
        export CXX="g++"
    fi
else
    echo "==> Using Clang via LLVM toolchain"
    CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${SOURCE_DIR}/cmake/llvm_toolchain.cmake")
fi

# ---------------------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------------------
echo "==> Configuring (${BUILD_TYPE})"
cmake "${CMAKE_ARGS[@]}"

# ---------------------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------------------
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo "==> Building with ${NPROC} jobs"
cmake --build "${BUILD_DIR}" -j "${NPROC}"

# ---------------------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------------------
if ${BUILD_TESTS}; then
    echo "==> Running tests"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure -j "${NPROC}"
fi

echo "==> Done"
