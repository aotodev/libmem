cmake_minimum_required(VERSION 3.30 FATAL_ERROR)

# ---------------------------------------------------------------------------------------
# Enable cmake experimental standard modules
# ---------------------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(enable_standard_modules)

enable_experimental_std()

# ---------------------------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------------------------
project(libmem LANGUAGES CXX)

# ---------------------------------------------------------------------------------------
# Compiler requirements: Clang >= 22 or GCC >= 15
# ---------------------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "22.1")
        message(FATAL_ERROR
            "Clang >= 22.1 is required for C++26 module support. "
            "Detected version: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    message(STATUS "[libmem] Compiler: Clang ${CMAKE_CXX_COMPILER_VERSION}")

elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "15")
        message(FATAL_ERROR
            "GCC >= 15 is required for C++26 module support. "
            "Detected version: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    message(STATUS "[libmem] Compiler: GCC ${CMAKE_CXX_COMPILER_VERSION}")

else()
    message(FATAL_ERROR
        "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. "
        "libmem requires Clang >= 22.1 or GCC >= 15.")
endif()

# ---------------------------------------------------------------------------------------
# Options
#
# USE_SANITIZERS / THREAD_SANITIZER are deliberately unprefixed: when libmem is
# embedded in a parent project, the sanitizer flags MUST match the parent's or the
# combined binary is ill-formed, so inheriting the parent's cache value is correct.
# LIBMEM_BUILD_TESTS / LIBMEM_BUILD_FUZZERS are prefixed so a parent's BUILD_TESTS=ON
# does not silently drag libmem's own suites into the consumer's build.
# ---------------------------------------------------------------------------------------
option(BUILD_SHARED_LIBS     "Build libmem as a shared library"                          OFF)
option(LIBMEM_PIC            "Enable Position Independent Code for static library"       OFF)
option(USE_SANITIZERS        "Enable ASan/UBSan in Debug mode"                           ON)
option(THREAD_SANITIZER      "Enable ThreadSanitizer instead of ASan"                    OFF)
option(LIBMEM_BUILD_TESTS    "Build unit tests"                                          OFF)
option(LIBMEM_BUILD_FUZZERS  "Build libFuzzer targets (Clang + Debug + sanitizers only)" OFF)
option(LIBMEM_USE_CCACHE     "Enable ccache if installed"                                OFF)

include(CMakeDependentOption)

cmake_dependent_option(USE_SANITIZERS
    "Enable sanitizers in Debug mode" ON "NOT WIN32" OFF)

cmake_dependent_option(THREAD_SANITIZER
    "Use Thread Sanitizer instead of ASan" OFF "USE_SANITIZERS" OFF)

# ---------------------------------------------------------------------------------------
# Global flags and properties
#
# Note: 'import std;' compiles the std module BMI with these flags. Any mismatch
# (e.g. std with RTTI, consumer without) causes link failures, which is exactly why
# this whole block is skipped when libmem is embedded via add_subdirectory /
# FetchContent. Global codegen policy belongs to the top-level project; imposing our
# own -fno-rtti / LTO / debug flags on a subtree of someone else's build would fork
# the std BMI and can produce an ODR-incompatible mix. An embedding parent is
# responsible for setting these consistently (libpixel, for instance, already does).
# ---------------------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_CXX_MODULE_STD ON)
    set(CMAKE_CXX_SCAN_FOR_MODULES ON)

    # Default to hidden visibility; shared builds export via LIBMEM_EXPORT
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

    # Disable RTTI: not needed, and reduces binary size
    add_compile_options(-fno-rtti)

    # Sized deallocation (both Clang and GCC support this)
    add_compile_options(-fsized-deallocation)

    # Debug flags
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(
            -Wno-reserved-module-identifier
            "$<$<CONFIG:Debug>:-Og;-fno-omit-frame-pointer;-fno-limit-debug-info;-g3;-glldb>"
            $<$<CONFIG:Release>:-flto>
        )
        add_link_options(
            $<$<CONFIG:Release>:-flto>
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(
            "$<$<CONFIG:Debug>:-Og;-fno-omit-frame-pointer;-g3;-ggdb>"
            $<$<CONFIG:Release>:-flto=auto>
        )
        add_link_options(
            $<$<CONFIG:Release>:-flto=auto>
        )
    endif()
endif()

# ---------------------------------------------------------------------------------------
# ccache
# ---------------------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL AND LIBMEM_USE_CCACHE)
    find_program(CCACHE "ccache")
    if(CCACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
    endif()
endif()

# ---------------------------------------------------------------------------------------
# Library type
# ---------------------------------------------------------------------------------------
if(BUILD_SHARED_LIBS)
    add_library(${PROJECT_NAME} SHARED)
else()
    add_library(${PROJECT_NAME} STATIC)
    if(LIBMEM_PIC)
        set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

# Language level and module support are set per-target rather than via the global
# CMAKE_CXX_* variables, so libmem stays self-sufficient when embedded in a parent
# project that skips the top-level block above.
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_MODULE_STD ON
    CXX_SCAN_FOR_MODULES ON
)

# ---------------------------------------------------------------------------------------
# Symbol visibility / export macros
# ---------------------------------------------------------------------------------------
if(BUILD_SHARED_LIBS)
    target_compile_definitions(${PROJECT_NAME} PUBLIC LIBMEM_SHARED)
    target_compile_definitions(${PROJECT_NAME} PRIVATE LIBMEM_BUILDING)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ---------------------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------------------
add_library(libmem_project_flags INTERFACE)

target_compile_options(libmem_project_flags INTERFACE
    -Wall
    -Wpedantic
    -Wextra
    -Wshadow
    -Wconversion
    -Wnarrowing
    -Wfloat-conversion
    -Wsign-conversion
    -Wold-style-cast
    -Wuninitialized
    -Werror

    # -Wundef is deliberately absent. These flags also compile the synthesized
    # 'import std;' BMI (see the note above), and libstdc++'s bits/std.cc guards
    # optional features with `#if __cpp_lib_senders` / `#if __glibcxx_reflection`
    # on macros that are simply not defined yet. With -Werror that makes the
    # std module fail to scan, so the library cannot build at all on GCC 16.
)

# Compiler-specific warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(libmem_project_flags INTERFACE
        -Wmove
        -Wno-reference-tu-local-entity-in-other-tu
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(libmem_project_flags INTERFACE
        -Wpessimizing-move
        -Wredundant-move
    )
endif()

# ---------------------------------------------------------------------------------------
# Sanitizers: kept in a separate target from the warnings above because the two have
# opposite embedding rules: instrumentation is worth keeping when libmem is built inside
# someone else's tree (it is a memory library; ASan on the allocators is the point),
# whereas our -Werror set is not (see below).
# ---------------------------------------------------------------------------------------
add_library(libmem_sanitizers INTERFACE)

if(USE_SANITIZERS)
    target_compile_options(libmem_sanitizers INTERFACE
        $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${THREAD_SANITIZER}>>>:-fsanitize=address,undefined>
        $<$<AND:$<CONFIG:Debug>,$<BOOL:${THREAD_SANITIZER}>>:-fsanitize=thread,undefined>
        # Make UBSan findings abort (and emit a stack trace) instead of merely
        # logging and continuing, so tests/CI actually fail on undefined behaviour.
        $<$<CONFIG:Debug>:-fno-sanitize-recover=undefined>
    )
    target_link_options(libmem_sanitizers INTERFACE
        $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${THREAD_SANITIZER}>>>:-fsanitize=address,undefined>
        $<$<AND:$<CONFIG:Debug>,$<BOOL:${THREAD_SANITIZER}>>:-fsanitize=thread,undefined>
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE libmem_sanitizers)

# Our warning policy applies to our own builds only. Forcing -Werror on a consumer is a
# packaging hazard: a newer compiler that emits a brand-new warning inside libmem would
# break *their* build through no fault of theirs. Embedded, libmem compiles with whatever
# warning flags the parent project uses.
if(PROJECT_IS_TOP_LEVEL)
    target_link_libraries(${PROJECT_NAME} PRIVATE libmem_project_flags)
endif()

# ---------------------------------------------------------------------------------------
# Module interface files
# ---------------------------------------------------------------------------------------
set(ModuleInterfaceFiles
    src/libmem/concepts.cppm
    src/libmem/slab.cppm
    src/libmem/multislab.cppm
    src/libmem/arena.cppm
    src/libmem/typed_arena.cppm
    src/libmem/pool.cppm
    src/libmem/sparse_set.cppm
    src/libmem/libmem.cppm
)

target_sources(${PROJECT_NAME}
  PUBLIC
    FILE_SET CXX_MODULES FILES
      ${ModuleInterfaceFiles}
)

# ---------------------------------------------------------------------------------------
# Symlink compile_commands.json to source root for tooling.
# Top-level only: when embedded, CMAKE_SOURCE_DIR is the parent's root and this would
# clobber the consumer's own symlink.
# ---------------------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    if(UNIX)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                ${CMAKE_BINARY_DIR}/compile_commands.json
                ${CMAKE_SOURCE_DIR}/compile_commands.json
        )
    else()
        add_custom_target(copy-compile-commands ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_BINARY_DIR}/compile_commands.json
                ${CMAKE_SOURCE_DIR}/compile_commands.json
            COMMENT "Copying compile_commands.json to source directory"
        )
    endif()
endif()

# ---------------------------------------------------------------------------------------
# Tests: top-level only. A consumer building its own suites must not have libmem's
# pulled in (and its `unit_tests` target name would collide).
# ---------------------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL AND LIBMEM_BUILD_TESTS)
    # enable_testing() must run in the top-level scope so CMake emits a root
    # CTestTestfile.cmake that descends into tests/; otherwise `ctest` invoked
    # at the build root discovers nothing (the tests/ include(CTest) only
    # enables testing for that subdirectory).
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------------------
# Fuzzing (libFuzzer)
#
# Coverage-guided fuzz targets. Reuses the Debug ASan + UBSan instrumentation and
# adds SanitizerCoverage to the library (and harnesses) via -fsanitize=fuzzer-no-link;
# the fuzz executables link the libFuzzer driver with -fsanitize=fuzzer.
# ---------------------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL AND LIBMEM_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "LIBMEM_BUILD_FUZZERS requires Clang (libFuzzer).")
    endif()
    if(NOT USE_SANITIZERS OR NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(FATAL_ERROR
            "LIBMEM_BUILD_FUZZERS needs a Debug build with USE_SANITIZERS=ON so ASan + UBSan are active. "
            "Configure with -DCMAKE_BUILD_TYPE=Debug -DLIBMEM_BUILD_FUZZERS=ON.")
    endif()

    # Instrument everything that links libmem_project_flags (the library and the
    # harnesses) for coverage. ASan + UBSan already come from the Debug block.
    target_compile_options(libmem_project_flags INTERFACE -fsanitize=fuzzer-no-link)

    add_subdirectory(fuzz)
endif()
