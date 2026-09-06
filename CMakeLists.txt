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
project(libmem VERSION 0.9.0 LANGUAGES CXX)

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
# USE_SANITIZERS / THREAD_SANITIZER: unprefixed on purpose, so an embedded build
# inherits the parent's value. Sanitizer flags have to match it.
# ---------------------------------------------------------------------------------------
option(BUILD_SHARED_LIBS     "Build libmem as a shared library"                          OFF)
option(LIBMEM_PIC            "Enable Position Independent Code for static library"       OFF)
option(USE_SANITIZERS        "Enable ASan/UBSan in Debug mode"                           ON)
option(THREAD_SANITIZER      "Enable ThreadSanitizer instead of ASan"                    OFF)
option(LIBMEM_BUILD_TESTS    "Build unit tests"                                          OFF)
option(LIBMEM_BUILD_FUZZERS  "Build libFuzzer targets (Clang + Debug + sanitizers only)" OFF)
option(LIBMEM_USE_CCACHE     "Enable ccache if installed"                                OFF)
option(LIBMEM_INSTALL        "Generate install rules and a find_package config"          ${PROJECT_IS_TOP_LEVEL})

include(CMakeDependentOption)

cmake_dependent_option(USE_SANITIZERS
    "Enable sanitizers in Debug mode" ON "NOT WIN32" OFF)

cmake_dependent_option(THREAD_SANITIZER
    "Use Thread Sanitizer instead of ASan" OFF "USE_SANITIZERS" OFF)

# ---------------------------------------------------------------------------------------
# Global flags and properties. Top-level only: these also compile the `import std;`
# BMI, so an embedded build must take the parent's codegen flags, not ours.
# ---------------------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_CXX_MODULE_STD ON)
    set(CMAKE_CXX_SCAN_FOR_MODULES ON)

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

# Same name a find_package consumer gets, so both paths link the same string.
add_library(libmem::libmem ALIAS ${PROJECT_NAME})

if(BUILD_SHARED_LIBS)
    # 0.x: every minor is an ABI break, so the soname carries major.minor.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    )
endif()

# Per-target, so libmem still compiles when the block above is skipped.
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_MODULE_STD ON
    CXX_SCAN_FOR_MODULES ON
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
    # No -Wundef: it makes libstdc++'s bits/std.cc fail to scan, so nothing builds.
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
# Sanitizers. Separate from the warnings above: this one is linked even when embedded.
# ---------------------------------------------------------------------------------------
add_library(libmem_sanitizers INTERFACE)

if(USE_SANITIZERS)
    target_compile_options(libmem_sanitizers INTERFACE
        $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${THREAD_SANITIZER}>>>:-fsanitize=address,undefined>
        $<$<AND:$<CONFIG:Debug>,$<BOOL:${THREAD_SANITIZER}>>:-fsanitize=thread,undefined>
        # Abort on a UBSan finding instead of logging and continuing.
        $<$<CONFIG:Debug>:-fno-sanitize-recover=undefined>
    )
    target_link_options(libmem_sanitizers INTERFACE
        $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${THREAD_SANITIZER}>>>:-fsanitize=address,undefined>
        $<$<AND:$<CONFIG:Debug>,$<BOOL:${THREAD_SANITIZER}>>:-fsanitize=thread,undefined>
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE libmem_sanitizers)

# Top-level only: never impose -Werror on a consumer's build.
if(PROJECT_IS_TOP_LEVEL)
    target_link_libraries(${PROJECT_NAME} PRIVATE libmem_project_flags)
endif()

# ---------------------------------------------------------------------------------------
# Module interface files
# ---------------------------------------------------------------------------------------
set(ModuleInterfaceFiles
    src/libmem/concepts.cppm
    src/libmem/identifier.cppm
    src/libmem/storage.cppm
    src/libmem/slab.cppm
    src/libmem/multislab.cppm
    src/libmem/arena.cppm
    src/libmem/typed_arena.cppm
    src/libmem/pool.cppm
    src/libmem/allocator.cppm
    src/libmem/spsc_ring.cppm
    src/libmem/vector.cppm
    src/libmem/sparse_index.cppm
    src/libmem/sparse_set.cppm
    src/libmem/libmem.cppm
)

target_sources(${PROJECT_NAME}
  PUBLIC
    FILE_SET CXX_MODULES
      BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
      FILES ${ModuleInterfaceFiles}
)

# ---------------------------------------------------------------------------------------
# Install / package
# libmem_project_flags and libmem_sanitizers land in INTERFACE_LINK_LIBRARIES under
# $<LINK_ONLY:> despite being PRIVATE; install(EXPORT) rejects the set without them.
# ---------------------------------------------------------------------------------------
if(LIBMEM_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(LIBMEM_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/libmem")

    # cmake-lint predates FILE_SET and reads its DESTINATION as a duplicate
    # keyword; cmake rejects splitting the call ("not all of its interface file
    # sets are installed").
    # cmake-lint: disable=E1122
    install(TARGETS ${PROJECT_NAME} libmem_project_flags libmem_sanitizers
        EXPORT libmem-targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        # Module interfaces install as sources: the consumer builds their own BMIs.
        FILE_SET CXX_MODULES DESTINATION "${CMAKE_INSTALL_DATADIR}/libmem/modules"
    )

    install(EXPORT libmem-targets
        FILE libmem-targets.cmake
        NAMESPACE libmem::
        DESTINATION "${LIBMEM_INSTALL_CMAKEDIR}"
        CXX_MODULES_DIRECTORY cxx-modules
    )

    # Consumers need this before their own project() call, so it ships separately.
    install(FILES cmake/enable_standard_modules.cmake
        DESTINATION "${LIBMEM_INSTALL_CMAKEDIR}")

    # MIT asks for the notice to travel with copies; a package is a copy.
    install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DOCDIR}")

    configure_package_config_file(
        cmake/libmemConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/libmemConfig.cmake"
        INSTALL_DESTINATION "${LIBMEM_INSTALL_CMAKEDIR}")

    # SameMinorVersion: pre-1.0, a minor bump is a break.
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/libmemConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/libmemConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/libmemConfigVersion.cmake"
        DESTINATION "${LIBMEM_INSTALL_CMAKEDIR}")
endif()

# ---------------------------------------------------------------------------------------
# Tests. Top-level only: a consumer must not get our suites, and `unit_tests` would
# collide with theirs.
# ---------------------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL AND LIBMEM_BUILD_TESTS)
    # Must stay in this scope: it is what makes ctest at the build root descend
    # into tests/. Moving it into tests/ silently discovers nothing.
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------------------
# Fuzzing (libFuzzer). Clang + Debug + sanitizers only.
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
