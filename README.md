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
one without touching its own logic.

```cpp
libmem::spsc_ring<command, 64> small{};              // slots inline, no allocation
libmem::heap_spsc_ring<command, 1 << 20> big{};      // same ring, slots on the heap

libmem::small_vector<hit, 8> hits{};                 // allocates nothing until the 9th hit

libmem::arena scratch{1 << 20};
libmem::sparse_set<entity, libmem::dynamic_storage<entity, libmem::resource_ref<libmem::arena>>>
    ids{libmem::resource_ref{scratch}};              // grows out of the arena
```

**Failure is a return value, not an exception.** Resources report exhaustion by
returning `nullptr`, and the containers pass that through: `push_back` returns a
`T*`, `reserve` returns `bool`, `insert` returns a result you can test. Nothing
here throws `std::bad_alloc` at you.

**No hidden deep copies.** No container is implicitly copyable, because a copy
constructor has no way to report that the resource ran out. Copying is explicit and
fallible instead, the way Rust makes it explicit and infallible:

```cpp
libmem::inline_vector<int, 8> b{a.clone()};              // fixed extent, cannot fail
if (auto c = heap_vec.try_clone()) { use(*c); }          // can fail, and says so
```

Everything moves, though, inline buffers included, and every move is `noexcept`.
A non-movable container is a standing tax on generic code.

**Growth cannot corrupt a container.** Allocating a new block, relocating into it,
and committing to it are three separate steps, so a failed growth (or a throwing
move constructor halfway through) leaves the container exactly as it was.

**Over-alignment is checked, not assumed.** A resource that cannot express an
alignment will not compile against an over-aligned request rather than silently
under-aligning.

**The inline containers work at compile time.** `inline_vector`, `sparse_set`, and
`sparse_map` over inline storage constant-evaluate end to end whenever the element
type is trivially default-constructible and trivially destructible, at no cost in
size or alignment:

```cpp
constexpr int total = [] {
    libmem::inline_vector<int, 8> v{};
    v.push_back(10);
    v.push_back(20);
    v.erase(v.begin());
    return v[0];
}();
static_assert(total == 20);
```

**It interoperates both ways.** A `std::pmr::polymorphic_allocator` can back a
libmem container, and `resource_allocator` turns any libmem resource into a standard
Allocator, so an `arena` can back a `std::vector`, `std::list`, or `std::map`.

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

## Planned

- `freelist`: intrusive free-list allocator for variable-size blocks.
- libFuzzer harnesses for `basic_vector` and for the paged sparse index.
- A `shrink_to_fit` on `basic_vector`, which needs a shrink step in the storage
  block protocol before `small_storage` could unspill.
