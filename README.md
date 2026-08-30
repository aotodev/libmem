# libmem

[![gcc](https://github.com/aotodev/libmem/actions/workflows/gcc.yml/badge.svg?branch=master)](https://github.com/aotodev/libmem/actions/workflows/gcc.yml)
[![clang](https://github.com/aotodev/libmem/actions/workflows/clang.yml/badge.svg?branch=master)](https://github.com/aotodev/libmem/actions/workflows/clang.yml)
[![fuzz](https://github.com/aotodev/libmem/actions/workflows/fuzz.yml/badge.svg?branch=master)](https://github.com/aotodev/libmem/actions/workflows/fuzz.yml)

A self-contained C++ memory allocator and container library. Built entirely
with C++20/23 modules (`import std`), serving also as a testbed for idiomatic
modern C++: concepts, deducing `this`, `std::ranges` integration, modules-only
builds, and constant evaluation wherever the object model permits it.

Compiler support for C++26 modules is still evolving; the library targets
Clang >= 22 and GCC >= 15.

## What's here

| Component | Description |
|-----------|-------------|
| `slab` | Fixed-size block allocator with compile-time bitmap tracking. |
| `multislab` | Auto-expanding chain of slabs with hysteresis-based shrink policy. |
| `arena` | Bump/region allocator for trivially-destructible types. |
| `typed_arena` | Bump allocator with LIFO destructor chain for arbitrary types. |
| `pool` | Pointer-stable typed container (bitmap-based object pool) over `multislab`. |
| `spsc_ring` | Lock-free single-producer single-consumer ring buffer with cached indices; `constinit`-able. |
| `vector` | Contiguous growable sequence. |
| `small_vector` | Same, holding its first `N` elements inline and spilling past them. |
| `inline_vector` | Same, bounded at `N` inline elements; allocates nothing, ever, and works at compile time. |
| `constexpr_inline_vector` | Same, for an element type that is `constexpr` default-constructible but not trivially so; default-initialises its slots to get there. |
| `fixed_vector` | Same, bounded at `N` with the slots on a resource instead of inline. |
| `sparse_set` | Dense-packed id set, O(1) insert / erase / contains, contiguous iteration. |
| `sparse_map` | `sparse_set` plus a payload per id; `keys()` and `values()` stay index-aligned. |
| `resource_allocator` | Standard Allocator over any libmem resource, so an `arena` can back a `std::vector`. |
| `allocator_resource` | The mirror: a standard Allocator backing a libmem container. |

Everything lives in a single module (`import libmem;`) with partitions.

## What makes it different

**Containers do not hard-code their buffer.** Each takes a `storage` argument that
owns raw space and nothing else, so one container serves an inline buffer, a
resource-backed fixed buffer, a growing heap buffer, or a small-buffer-optimised
one without touching its own logic. The four kinds, the growth protocol and the
resource interface are in [docs/storage.md](docs/storage.md).

```cpp
libmem::spsc_ring<command, 64> small{};              // slots inline, no allocation
libmem::heap_spsc_ring<command, 1 << 20> big{};      // same ring, slots on the heap

libmem::small_vector<hit, 8> hits{};                 // allocates nothing until the 9th hit
```

**Failure is a return value, not an exception.** `push_back` returns a `T*`,
`reserve` returns `bool`, and nothing throws `std::bad_alloc`. No container is
implicitly copyable either, since a copy constructor could not report the
failure; copying is explicit through `clone()` and `try_clone()`. Everything
moves, inline buffers included, and every move is `noexcept` unless the element
type's own default constructor can throw. See
[docs/containers.md](docs/containers.md).

**The inline containers work at compile time.** `inline_vector`, `sparse_set`
and `sparse_map` over inline storage constant-evaluate end to end for trivially
default-constructible and trivially destructible element types, at no cost in
size or alignment. `constexpr_inline_vector` opts an element type whose default
constructor is `constexpr` but not trivial into the same thing, for the price of
running it once per slot on every such vector built, moved or cloned.

**It interoperates both ways.** A `std::pmr::polymorphic_allocator` can back a
libmem container, and `resource_allocator` turns any libmem resource into a
standard Allocator, so an `arena` can back a `std::vector` or `std::map`.

Intended for the places a general-purpose allocator is the wrong shape: an ECS
storing components by entity id, a frame arena reset every tick, a lock-free
command queue between two threads, hot paths where a `std::vector` of eight
elements should not touch the heap.

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

To use it from another project, see [docs/integration.md](docs/integration.md).

## Tested and fuzzed

CI builds and runs the suite with **both GCC and Clang** on every push, under
**AddressSanitizer + UndefinedBehaviorSanitizer**, with the ring handoff also run
under **ThreadSanitizer**. Coverage-guided **libFuzzer** harnesses drive
`multislab`, `sparse_set`, and `sparse_map`; the two sparse ones are differential,
comparing every operation against a standard container.

This is an early-stage library, so treat that as what is exercised today rather
than a guarantee of exhaustive coverage. Details in
[docs/testing.md](docs/testing.md).

## Docs

- [Storage](docs/storage.md): the four storage kinds, resources, the growth protocol, small-buffer support, constant evaluation, standard-container interop.
- [Containers](docs/containers.md): the vector family, identifiers, sparse set and map, copying and moving, ranges interop, constant evaluation.
- [Complexity](docs/complexity.md): cost tables for every component.
- [Testing](docs/testing.md): what the suites and fuzzers actually cover.
- [Integration](docs/integration.md): consuming libmem from another CMake project.
