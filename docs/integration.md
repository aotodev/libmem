# Using libmem from another project

```cmake
include(FetchContent)
FetchContent_Declare(
    libmem
    GIT_REPOSITORY https://github.com/aotodev/libmem.git
    GIT_TAG master
    SYSTEM
)
FetchContent_MakeAvailable(libmem)

target_link_libraries(my_target PRIVATE libmem)
```

## Staying out of the consumer's way

When libmem is not the top-level project it does not touch the global
`CMAKE_CXX_*` variables, add `add_compile_options`, wire ccache, symlink
`compile_commands.json`, or build its own tests and fuzzers. The library target
carries its own `CXX_STANDARD` / `CXX_MODULE_STD` properties, so it compiles
correctly regardless.

That deliberately leaves **global codegen policy to the parent**. `import std;`
builds the std module BMI from whatever flags a target uses, so the consumer should
set options like `-fno-rtti`, LTO, and sanitizers project-wide, and have libmem and
the rest of the build agree. A mismatch forks the std BMI at best and is ill-formed
at worst.

## Option naming

`USE_SANITIZERS` and `THREAD_SANITIZER` are intentionally **not** prefixed:
inheriting the parent's values is the correct behaviour, for the reason above.

Everything else is namespaced so it cannot collide with a consumer's own names:

| Option | Default | Effect |
|--------|---------|--------|
| `LIBMEM_BUILD_TESTS` | `OFF` | Build the GoogleTest suites |
| `LIBMEM_BUILD_FUZZERS` | `OFF` | Build the libFuzzer harnesses (Clang + Debug + sanitizers) |
| `LIBMEM_USE_CCACHE` | `OFF` | Use ccache if installed |
| `LIBMEM_PIC` | `OFF` | Position-independent code for the static library |

The internal warnings target is `libmem_project_flags` for the same reason, and it
is linked only when libmem is the top-level project. Forcing `-Werror` on a
consumer is a packaging hazard: a newer compiler emitting a brand-new warning
inside libmem would break their build through no fault of theirs.

`libmem_sanitizers` is linked unconditionally, because instrumentation is worth
keeping when libmem is built inside someone else's tree. It is a memory library;
ASan on the allocators is the point.
