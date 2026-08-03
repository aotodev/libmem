# libmem

[![gcc](https://github.com/aotodev/libmem/actions/workflows/gcc.yml/badge.svg?branch=master)](https://github.com/aotodev/libmem/actions/workflows/gcc.yml)
[![clang](https://github.com/aotodev/libmem/actions/workflows/clang.yml/badge.svg?branch=master)](https://github.com/aotodev/libmem/actions/workflows/clang.yml)
[![fuzz](https://github.com/aotodev/libmem/actions/workflows/fuzz.yml/badge.svg?branch=master)](https://github.com/aotodev/libmem/actions/workflows/fuzz.yml)

A small, self-contained C++ memory allocator and container library. Built
entirely with C++23/26 modules (`import std`), serving also as a testbed for
idiomatic modern C++ -- concepts, `constexpr` everything, deducing `this`,
`std::ranges` integration, modules-only builds.

Compiler support for C++26 modules is still evolving; the library targets
Clang >= 22 and GCC >= 15.

## What's here

| Component    | Description |
|--------------|-------------|
| `slab`       | Fixed-size block allocator with compile-time bitmap tracking. |
| `multislab`  | Auto-expanding chain of slabs with hysteresis-based shrink policy. |
| `arena`      | Bump/region allocator for trivially-destructible types. |
| `typed_arena` | Bump allocator with LIFO destructor chain for arbitrary types. |
| `pool`       | Pointer-stable typed container (bitmap-based object pool) over `multislab`. |

Everything lives in a single module (`import libmem;`) with partitions.

## Complexity

| Operation | `slab` | `multislab` | `arena` / `typed_arena` | `pool` |
|-----------|--------|-------------|-------------------------|--------|
| allocate  | O(N/64) | O(N/64) amortised | O(1) | O(N/64) amortised |
| deallocate | O(1) | O(S) | no-op | O(S) |
| iterate (skip empties) | O(N/64) per word | O(N/64) per word | n/a | O(N/64) per word |
| reset / clear | O(W) | O(S) | O(1) / O(D) | O(S + E) |

N = capacity in blocks, W = bitmap words (N/64), S = number of slab pages, D = registered destructors, E = live elements (for non-trivial dtors).

`find_owner` is O(S), a linear scan over slab pages, and is what makes
`deallocate` O(S) on `multislab` and `pool`.

Allocation avoids it entirely. `slab::allocate_at()` and `multislab::allocate_at()`
return the block *plus* its position (a bit-index and an iterator respectively),
both of which the allocator already had in hand. `pool::emplace` uses this, so
insertion stays O(N/64) amortised rather than paying an O(S) `find_owner` scan to
recover the position it just discarded. Reach for `make_iterator(ptr)` only when
you have a pointer and no allocation to go with it.

## Building

```sh
./scripts/make.sh              # Debug, Clang (default)
./scripts/make.sh --gcc        # Debug, GCC
./scripts/make.sh --release    # Release, Clang
./scripts/make.sh --test       # Build and run tests
./scripts/make.sh --shared     # Shared library
```

Or directly with CMake:

```sh
cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/llvm_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Debug \
    -DLIBMEM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Using it from another project

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

When libmem is not the top-level project it stays out of the consumer's way: it
does not touch the global `CMAKE_CXX_*` variables, add `add_compile_options`, wire
ccache, symlink `compile_commands.json`, or build its own tests and fuzzers. The
library target carries its own `CXX_STANDARD`/`CXX_MODULE_STD` properties, so it
compiles correctly regardless.

That deliberately leaves **global codegen policy to the parent**. Because
`import std;` builds the std module BMI from whatever flags a target uses, the
consumer should set options like `-fno-rtti`, LTO, and sanitizers project-wide so
libmem and the rest of the build agree, a mismatch forks the std BMI at best and
is ill-formed at worst. `USE_SANITIZERS` / `THREAD_SANITIZER` are intentionally
*not* prefixed for the same reason: inheriting the parent's values is correct.
Everything else is namespaced (`LIBMEM_BUILD_TESTS`, `LIBMEM_BUILD_FUZZERS`,
`LIBMEM_USE_CCACHE`, `LIBMEM_PIC`), and the internal warnings target is
`libmem_project_flags`, so neither collides with a consumer's own names.

## Testing

CI builds and runs the test suite with both **GCC and Clang** on every push
(see the badges), under **AddressSanitizer + UndefinedBehaviorSanitizer**, on
by default in Debug via the `USE_SANITIZERS` option, with UBSan set to abort on
the first finding. After configuring with `-DLIBMEM_BUILD_TESTS=ON` (see above), run
them with `ctest --test-dir build`.

The GoogleTest suites cover `slab`, `multislab`, `arena`, `typed_arena`, and
`pool`: allocation / exhaustion / reuse, slab growth and empty-slab hysteresis,
full/active list transitions, iteration, pointer stability, destructor ordering,
and leak balance (via a counting `memory_resource`).

A coverage-guided **libFuzzer** harness drives the `multislab` allocator under
ASan + UBSan (`-DLIBMEM_BUILD_FUZZERS=ON`, Clang only); CI runs a short, time-boxed
smoke pass. See [fuzz/README.md](fuzz/README.md).

This is an early-stage library (and a C++26 modules testbed), so treat the above
as what is exercised today, not a guarantee of exhaustive coverage.

## Planned

- `sparse_set`:  dense-packed unordered set with O(1) insert/remove/contains.
- `freelist`:  intrusive free-list allocator for variable-size blocks.
- `stack_arena`: fixed-capacity arena backed by stack storage (`std::array`).
- `ring_buffer: lock-free SPSC ring buffer over arena memory.
