# Using libmem from another project

libmem is a modules-only library, so both paths below compile its module interfaces
inside your build tree. That needs Clang >= 22.1 or GCC >= 15, C++26, and `import
std;` enabled in your project.

## FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    libmem
    GIT_REPOSITORY https://github.com/aotodev/libmem.git
    GIT_TAG v0.9.0
    SYSTEM
)
FetchContent_MakeAvailable(libmem)

target_link_libraries(my_target PRIVATE libmem::libmem)
```

A tag can be force-pushed, so `GIT_TAG` with a full commit sha is the reproducible
pin. `libmem::libmem` and plain `libmem` are the same target here; the namespaced
name is the one `find_package` also gives you.

## find_package

Install first:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/opt/libmem
cmake --build build
cmake --install build
```

Then, in the consumer:

```cmake
cmake_minimum_required(VERSION 3.30 FATAL_ERROR)

# Before project(): CMAKE_EXPERIMENTAL_CXX_IMPORT_STD is read when CXX is enabled,
# and find_package runs too late to set it for you. The helper is installed with
# the package and picks the uuid for your cmake version.
list(APPEND CMAKE_MODULE_PATH /opt/libmem/lib/cmake/libmem)
include(enable_standard_modules)
enable_experimental_std()

project(my_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)

find_package(libmem 0.9 REQUIRED)

target_link_libraries(my_target PRIVATE libmem::libmem)
```

The package installs module interfaces as sources under
`share/libmem/modules/libmem/`, never BMIs: your build compiles them with your own
flags, which is what keeps the std module consistent across the two.

Compatibility is `SameMinorVersion`. Pre-1.0 a minor bump is a break, so `0.9`
accepts 0.9.x and rejects 0.10.

### Static and shared

Both install, and static is the default.

The installed binary holds one initializer per module and nothing else. Every
entity libmem exports is a template or an explicitly `inline` function, so all of
it compiles in your tree under your flags: a consumer building with ASan
instruments libmem's allocators too, which a prebuilt archive silently prevents.

libmem therefore publishes no ABI, and `BUILD_SHARED_LIBS` buys close to nothing,
a `.so` of 14 initializer symbols. Prefer static.

Do not install a `Release` build unless the consumer also links with LTO. `Release`
adds `-flto`, which makes `liblibmem.a` an LLVM bitcode archive, and a plain link
against it fails with `file format not recognized`. `RelWithDebInfo` is the
optimized configuration that installs cleanly.

## Staying out of the consumer's way

When libmem is not the top-level project it does not touch the global
`CMAKE_CXX_*` variables, add `add_compile_options`, wire ccache, symlink
`compile_commands.json`, or build its own tests and fuzzers. The library target
carries its own `CXX_STANDARD` / `CXX_MODULE_STD` properties, so it compiles
correctly regardless.

Symbol visibility is the parent's too: libmem sets no visibility preset, because
visibility is a property of the final linked binary and every consumption path
compiles the module interfaces in the consumer's tree.

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
| `LIBMEM_INSTALL` | top-level | Generate install rules and the find_package config |

The internal warnings target is `libmem_project_flags` for the same reason, and it
is linked only when libmem is the top-level project. Forcing `-Werror` on a
consumer is a packaging hazard: a newer compiler emitting a brand-new warning
inside libmem would break their build through no fault of theirs.

`libmem_sanitizers` is linked unconditionally, because instrumentation is worth
keeping when libmem is built inside someone else's tree. It is a memory library;
ASan on the allocators is the point.

Both are in the export set, because cmake records a library's private link
dependencies under `$<LINK_ONLY:>`; `install(EXPORT)` refuses the set otherwise. A
`find_package` consumer therefore sees `libmem::libmem_project_flags` and
`libmem::libmem_sanitizers`, but `LINK_ONLY` means it inherits their link
requirements only, never `-Werror`.
