# ---------------------------------------------------------------------------------------
# llvm_toolchain.cmake — Universal LLVM 22+ Toolchain File (Multi-Distro)
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/llvm_toolchain.cmake ..
#
# supports: ubuntu/debian, arch, fedora, gentoo, macos (homebrew)
# ---------------------------------------------------------------------------------------

# Guard against toolchain re-entry (CMake processes this file multiple times)
if(_LLVM_TOOLCHAIN_RESOLVED)
    return()
endif()

# ---------------------------------------------------------------------------------------
# configuration
# ---------------------------------------------------------------------------------------
set(_LLVM_REQUIRED_VERSION "22.1.0")
set(_LLVM_MAJOR_VERSION   "22")

# Common search paths for versioned LLVM installs.
# macOS Homebrew paths are listed FIRST so that the upstream LLVM clang++ is
# found before /usr/bin/clang++ (which is AppleClang on macOS).
set(_LLVM_SEARCH_PATHS
    "/opt/homebrew/opt/llvm/bin"                 # macos homebrew (apple silicon)
    "/usr/local/opt/llvm/bin"                    # macos homebrew (intel)
    "/usr/lib/llvm-${_LLVM_MAJOR_VERSION}/bin"   # ubuntu / debian
    "/usr/bin"                                   # arch / fedora / gentoo
)

# ---------------------------------------------------------------------------------------
# find a versioned LLVM tool
#
# For CMAKE_<LANG>_COMPILER variables, CMake toolchain files MUST use set()
# rather than find_program() directly into those variables. find_program()
# only creates a CACHE entry, but CMakeDetermineCompiler.cmake checks the
# normal variable first — if it's unset, CMake ignores the cache and finds
# the compiler on its own (often picking the wrong one, e.g. AppleClang).
# ---------------------------------------------------------------------------------------
macro(_find_llvm_tool VAR VERSIONED_NAME GENERIC_NAME)
    set(_find_result "_LLVM_FOUND_${VAR}")
    # First pass: search only our known paths
    find_program(${_find_result}
        NAMES ${VERSIONED_NAME} ${GENERIC_NAME}
        HINTS ${_LLVM_SEARCH_PATHS}
        NO_DEFAULT_PATH
    )
    # Second pass: fall back to CMake/system PATH
    if(NOT ${_find_result})
        find_program(${_find_result} NAMES ${VERSIONED_NAME} ${GENERIC_NAME})
    endif()
    if(NOT ${_find_result})
        message(FATAL_ERROR
            "[LLVM Toolchain] Could not find '${VERSIONED_NAME}' or '${GENERIC_NAME}' "
            "in any of: ${_LLVM_SEARCH_PATHS} or system PATH.")
    endif()
    set(${VAR} "${${_find_result}}")
    unset(_find_result)
endmacro()

# ---------------------------------------------------------------------------------------
# locate compilers
# ---------------------------------------------------------------------------------------
_find_llvm_tool(CMAKE_C_COMPILER     clang-${_LLVM_MAJOR_VERSION}       clang)
_find_llvm_tool(CMAKE_CXX_COMPILER   clang++-${_LLVM_MAJOR_VERSION}     clang++)

# ---------------------------------------------------------------------------------------
# verify version >= 22.1
# ---------------------------------------------------------------------------------------
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" --version
    OUTPUT_VARIABLE _clang_version_raw
    RESULT_VARIABLE _clang_version_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT _clang_version_result EQUAL 0)
    message(FATAL_ERROR
        "[LLVM Toolchain] Failed to execute: ${CMAKE_C_COMPILER} --version")
endif()

string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _clang_version "${_clang_version_raw}")

if(NOT _clang_version OR _clang_version VERSION_LESS "${_LLVM_REQUIRED_VERSION}")
    message(FATAL_ERROR
        "[LLVM Toolchain] Clang >= ${_LLVM_REQUIRED_VERSION} required, "
        "found '${_clang_version}' at ${CMAKE_C_COMPILER}")
endif()

message(STATUS "[LLVM Toolchain] Clang ${_clang_version}: ${CMAKE_C_COMPILER}")

# ---------------------------------------------------------------------------------------
# Locate LLVM binutils
# ---------------------------------------------------------------------------------------

_find_llvm_tool(CMAKE_AR       llvm-ar-${_LLVM_MAJOR_VERSION}       llvm-ar)
_find_llvm_tool(CMAKE_RANLIB   llvm-ranlib-${_LLVM_MAJOR_VERSION}   llvm-ranlib)
_find_llvm_tool(CMAKE_NM       llvm-nm-${_LLVM_MAJOR_VERSION}       llvm-nm)
_find_llvm_tool(CMAKE_OBJCOPY  llvm-objcopy-${_LLVM_MAJOR_VERSION}  llvm-objcopy)
_find_llvm_tool(CMAKE_OBJDUMP  llvm-objdump-${_LLVM_MAJOR_VERSION}  llvm-objdump)
_find_llvm_tool(CMAKE_READELF  llvm-readelf-${_LLVM_MAJOR_VERSION}  llvm-readelf)
_find_llvm_tool(CMAKE_STRIP    llvm-strip-${_LLVM_MAJOR_VERSION}    llvm-strip)

# ---------------------------------------------------------------------------------------
# Standard library: libc++ and lld linker
#
# Clang defaults to libstdc++ on Linux. We explicitly select libc++ and let
# lld pull in libc++abi and libunwind automatically.
#
# On Ubuntu/Debian, versioned installs (clang-22) put headers and libs under
# /usr/lib/llvm-<N>/. We detect this and add the paths automatically.
# ---------------------------------------------------------------------------------------
_find_llvm_tool(_LLD_LINKER ld.lld-${_LLVM_MAJOR_VERSION} ld.lld)
message(STATUS "[LLVM Toolchain] LLD: ${_LLD_LINKER}")

set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-stdlib=libc++ -fuse-ld=lld")

# Detect whether the compiler lives under a versioned LLVM prefix.
# e.g. /usr/lib/llvm-22/bin/clang-22 → prefix = /usr/lib/llvm-22
cmake_path(GET CMAKE_C_COMPILER PARENT_PATH _CLANG_BIN_DIR)
cmake_path(GET _CLANG_BIN_DIR   PARENT_PATH _LLVM_PREFIX_CANDIDATE)

# Expose the prefix for downstream targets (e.g. static libc++ paths)
set(LLVM_PREFIX "${_LLVM_PREFIX_CANDIDATE}" CACHE PATH "LLVM install prefix")

set(_LIBCXX_HEADER  "${_LLVM_PREFIX_CANDIDATE}/include/c++/v1/__config")
set(_LIBCXX_LIBRARY "${_LLVM_PREFIX_CANDIDATE}/lib/libc++.so")

if(EXISTS "${_LIBCXX_HEADER}" AND EXISTS "${_LIBCXX_LIBRARY}")
    message(STATUS "[LLVM Toolchain] Versioned libc++ found under ${_LLVM_PREFIX_CANDIDATE}")

    string(APPEND CMAKE_CXX_FLAGS_INIT " -cxx-isystem ${_LLVM_PREFIX_CANDIDATE}/include/c++/v1")

    set(_LLVM_LIBDIR_FLAG " -L${_LLVM_PREFIX_CANDIDATE}/lib -Wl,-rpath,${_LLVM_PREFIX_CANDIDATE}/lib")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT    "${_LLVM_LIBDIR_FLAG}")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT "${_LLVM_LIBDIR_FLAG}")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT "${_LLVM_LIBDIR_FLAG}")

    # ---- C++23 import std: module source location ----------------------------------
    #
    # CMake needs to find std.cppm and std.compat.cppm to build the 'import std'
    # module. With versioned LLVM installs, these live under the LLVM prefix
    # rather than the default /usr/lib/... path. Without this hint, CMake
    # constructs a wrong path like /lib/share/libc++/v1/std.cppm.

    set(_LIBCXX_MODULE_DIR "${_LLVM_PREFIX_CANDIDATE}/share/libc++/v1")

    if(EXISTS "${_LIBCXX_MODULE_DIR}/std.cppm")
        set(CMAKE_CXX_MODULE_STD_SOURCE "${_LIBCXX_MODULE_DIR}/std.cppm" CACHE FILEPATH
            "Path to libc++ std.cppm for import std")
        message(STATUS "[LLVM Toolchain] C++23 std module: ${_LIBCXX_MODULE_DIR}/std.cppm")
    else()
        message(WARNING
            "[LLVM Toolchain] std.cppm not found at ${_LIBCXX_MODULE_DIR}/. "
            "'import std' may not work.")
    endif()

    unset(_LIBCXX_MODULE_DIR)
    unset(_LLVM_LIBDIR_FLAG)
else()
    message(STATUS "[LLVM Toolchain] libc++ in default search paths (non-versioned install)")
endif()

unset(_CLANG_BIN_DIR)
unset(_LLVM_PREFIX_CANDIDATE)
unset(_LIBCXX_HEADER)
unset(_LIBCXX_LIBRARY)

# ---------------------------------------------------------------------------------------
# mark resolved so re-entry is a no-op
# ---------------------------------------------------------------------------------------
set(_LLVM_TOOLCHAIN_RESOLVED TRUE CACHE INTERNAL
    "LLVM toolchain has been configured; skip on re-entry.")

message(STATUS "[LLVM Toolchain] Fully resolved — all tools from LLVM ${_LLVM_MAJOR_VERSION}")
