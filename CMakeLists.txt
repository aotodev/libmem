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
# ---------------------------------------------------------------------------------------
option(BUILD_SHARED_LIBS  "Build libmem as a shared library"                          OFF)
option(BUILD_TESTS        "Build unit tests"                                          OFF)
option(LIBMEM_PIC         "Enable Position Independent Code for static library"       OFF)
option(USE_SANITIZERS     "Enable ASan/UBSan in Debug mode"                           ON)
option(THREAD_SANITIZER   "Enable ThreadSanitizer instead of ASan"                    OFF)
option(USE_CCACHE         "Enable ccache if installed"                                OFF)

include(CMakeDependentOption)

cmake_dependent_option(USE_SANITIZERS
    "Enable sanitizers in Debug mode" ON "NOT WIN32" OFF)

cmake_dependent_option(THREAD_SANITIZER
    "Use Thread Sanitizer instead of ASan" OFF "USE_SANITIZERS" OFF)

# ---------------------------------------------------------------------------------------
# Global flags and properties
# Note: 'import std;' compiles the std module BMI with these flags.
# Any mismatch (e.g. std with RTTI, consumer without) causes link failures.
# ---------------------------------------------------------------------------------------
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# Default to hidden visibility — shared builds export via LIBMEM_EXPORT
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Disable RTTI — not needed and reduces binary size
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

# ---------------------------------------------------------------------------------------
# ccache
# ---------------------------------------------------------------------------------------
if(USE_CCACHE)
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
add_library(project_flags INTERFACE)

target_compile_options(project_flags INTERFACE
    -Wall
    -Wpedantic
    -Wextra
    -Wshadow
    -Wconversion
    -Wnarrowing
    -Wfloat-conversion
    -Wsign-conversion
    -Wold-style-cast
    -Wundef
    -Wuninitialized
    -Werror
)

# Compiler-specific warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(project_flags INTERFACE
        -Wmove
        -Wno-reference-tu-local-entity-in-other-tu
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(project_flags INTERFACE
        -Wpessimizing-move
        -Wredundant-move
    )
endif()

# Sanitizers
if(USE_SANITIZERS)
    target_compile_options(project_flags INTERFACE
        $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${THREAD_SANITIZER}>>>:-fsanitize=address,undefined>
        $<$<AND:$<CONFIG:Debug>,$<BOOL:${THREAD_SANITIZER}>>:-fsanitize=thread,undefined>
    )
    target_link_options(project_flags INTERFACE
        $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${THREAD_SANITIZER}>>>:-fsanitize=address,undefined>
        $<$<AND:$<CONFIG:Debug>,$<BOOL:${THREAD_SANITIZER}>>:-fsanitize=thread,undefined>
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE project_flags)

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
# Symlink compile_commands.json to source root for tooling
# ---------------------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
