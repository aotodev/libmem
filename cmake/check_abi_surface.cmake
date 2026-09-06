# Fails if libmem defines any strong symbol other than a module initializer.
# Weak definitions are fine: an internal template instantiation is COMDAT and
# costs a consumer nothing. A strong one means a non-template definition lost
# its `inline` and the library grew an ABI again.
# Invoked by the `abi_surface` test with -DNM and -DLIB.

execute_process(
    COMMAND "${NM}" -C --defined-only "${LIB}"
    OUTPUT_VARIABLE symbols
    RESULT_VARIABLE status
    ERROR_VARIABLE stderr)

if(NOT status EQUAL 0)
    message(FATAL_ERROR "nm failed on ${LIB}: ${stderr}")
endif()

string(REPLACE "\n" ";" lines "${symbols}")

set(unexpected "")
foreach(line IN LISTS lines)
    if(line MATCHES "^[0-9a-fA-F]+ +([TDB]) +(.+)$")
        # A nested MATCHES clobbers CMAKE_MATCH_*, so copy them out first.
        set(type "${CMAKE_MATCH_1}")
        set(name "${CMAKE_MATCH_2}")
        if(NOT name MATCHES "^initializer for module ")
            list(APPEND unexpected "  ${type} ${name}")
        endif()
    endif()
endforeach()

if(unexpected)
    list(JOIN unexpected "\n" report)
    message(FATAL_ERROR
        "${LIB} defines strong symbols beyond the module initializers:\n${report}\n"
        "Mark the definition `inline` (see the invariant comment on the class).")
endif()
