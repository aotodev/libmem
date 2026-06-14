# libmem

[![gcc](https://git.vernizzi.io/vernizzi/libmem/actions/workflows/gcc.yml/badge.svg?branch=master&label=gcc)](https://git.vernizzi.io/vernizzi/libmem/actions?workflow=gcc.yml)
[![clang](https://git.vernizzi.io/vernizzi/libmem/actions/workflows/clang.yml/badge.svg?branch=master&label=clang)](https://git.vernizzi.io/vernizzi/libmem/actions?workflow=clang.yml)
[![fuzz](https://git.vernizzi.io/vernizzi/libmem/actions/workflows/fuzz.yml/badge.svg?branch=master&label=fuzz)](https://git.vernizzi.io/vernizzi/libmem/actions?workflow=fuzz.yml)

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

`find_owner` during deallocation is O(S) -- a linear scan over slab pages.

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
    -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Testing

CI builds and runs the test suite with both **GCC and Clang** on every push
(see the badges), under **AddressSanitizer + UndefinedBehaviorSanitizer**, on
by default in Debug via the `USE_SANITIZERS` option, with UBSan set to abort on
the first finding. After configuring with `-DBUILD_TESTS=ON` (see above), run
them with `ctest --test-dir build`.

The GoogleTest suites cover `slab`, `multislab`, `arena`, `typed_arena`, and
`pool`: allocation / exhaustion / reuse, slab growth and empty-slab hysteresis,
full/active list transitions, iteration, pointer stability, destructor ordering,
and leak balance (via a counting `memory_resource`).

A coverage-guided **libFuzzer** harness drives the `multislab` allocator under
ASan + UBSan (`-DBUILD_FUZZERS=ON`, Clang only); CI runs a short, time-boxed
smoke pass. See [fuzz/README.md](fuzz/README.md).

This is an early-stage library (and a C++26 modules testbed), so treat the above
as what is exercised today, not a guarantee of exhaustive coverage.

## Planned

- `sparse_set` -- dense-packed unordered set with O(1) insert/remove/contains.
- `freelist` -- intrusive free-list allocator for variable-size blocks.
- `stack_arena` -- fixed-capacity arena backed by stack storage (`std::array`).
- `ring_buffer` -- lock-free SPSC ring buffer over arena memory.
