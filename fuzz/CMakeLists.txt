# ======================================================================================
# libFuzzer targets (enabled with -DLIBMEM_BUILD_FUZZERS=ON; Clang + Debug only).
#
# The library and these harnesses are already compiled with ASan + UBSan (Debug)
# and SanitizerCoverage (-fsanitize=fuzzer-no-link, added to libmem_project_flags by the
# LIBMEM_BUILD_FUZZERS block in the top-level CMakeLists). Each target links the libFuzzer
# driver via -fsanitize=fuzzer.
# ======================================================================================

include(CheckLinkerFlag)

# -fno-rtti leaves a non-PIC reloc against exception typeinfo that a PIE link rejects
# on some libc++ builds. Checked rather than assumed: PIE is mandatory on arm64 Darwin.
check_linker_flag(CXX -no-pie LIBMEM_HAVE_NO_PIE)

function(add_libmem_fuzzer name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE libmem libmem_project_flags libmem_sanitizers)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    if(LIBMEM_HAVE_NO_PIE)
        target_link_options(${name} PRIVATE -no-pie)
    endif()
endfunction()

add_libmem_fuzzer(fuzz_multislab)
add_libmem_fuzzer(fuzz_sparse_set)
add_libmem_fuzzer(fuzz_sparse_map)
