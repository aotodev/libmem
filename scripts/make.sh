#!/usr/bin/env bash
# ---------------------------------------------------------------------------------------
# make.sh: configure + build + (optionally) test libmem
#
# Usage:
#   ./scripts/make.sh                    # Default: Debug, Clang via LLVM toolchain
#   ./scripts/make.sh --release          # Release build
#   ./scripts/make.sh --gcc              # Use GCC instead of Clang
#   ./scripts/make.sh --test             # Build and run tests
#   ./scripts/make.sh --shared           # Build as shared library
#   ./scripts/make.sh --clean            # Remove build directory first
#   ./scripts/make.sh --clangd           # Build build-clangd/, the database .clangd reads
#   ./scripts/make.sh --release --test   # Combine flags as needed
# ---------------------------------------------------------------------------------------

set -euo pipefail

# ---------------------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------------------
BUILD_TYPE="Debug"
USE_GCC=false
BUILD_TESTS=false
RUN_TESTS=false
BUILD_SHARED=false
CLEAN=false
CLANGD=false

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
        --test)     BUILD_TESTS=true; RUN_TESTS=true ;;
        --shared)   BUILD_SHARED=true ;;
        --clean)    CLEAN=true ;;
        --clangd)   CLANGD=true ;;
        *)
            echo "Unknown option: ${arg}"
            echo "Usage: $0 [--release] [--gcc] [--test] [--shared] [--clean] [--clangd]"
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------------------
# clangd database: its own directory, because build/ alternates compilers with --gcc and
# clangd cannot read gcc's module flags. Tests are configured so their TUs get entries,
# and it is built rather than only configured: the commands carry -fmodule-file= paths,
# and clangd cannot produce the std BMI itself.
# ---------------------------------------------------------------------------------------
if ${CLANGD}; then
    BUILD_DIR="${SOURCE_DIR}/build-clangd"
    USE_GCC=false
    BUILD_TESTS=true
    RUN_TESTS=false
fi

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
    CMAKE_ARGS+=("-DLIBMEM_BUILD_TESTS=ON")
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
if ${RUN_TESTS}; then
    echo "==> Running tests"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure -j "${NPROC}"
fi

echo "==> Done"
