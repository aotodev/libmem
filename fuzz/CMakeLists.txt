# ======================================================================================
# libFuzzer targets (enabled with -DLIBMEM_BUILD_FUZZERS=ON; Clang + Debug only).
#
# The library and these harnesses are already compiled with ASan + UBSan (Debug)
# and SanitizerCoverage (-fsanitize=fuzzer-no-link, added to libmem_project_flags by the
# LIBMEM_BUILD_FUZZERS block in the top-level CMakeLists). Each target links the libFuzzer
# driver via -fsanitize=fuzzer.
# ======================================================================================

function(add_libmem_fuzzer name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE libmem libmem_project_flags libmem_sanitizers)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

add_libmem_fuzzer(fuzz_multislab)
add_libmem_fuzzer(fuzz_sparse_set)
add_libmem_fuzzer(fuzz_sparse_map)
