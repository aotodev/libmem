# Testing

CI builds and runs the suite with both **GCC and Clang** on every push, under
**AddressSanitizer + UndefinedBehaviorSanitizer**, on by default in Debug via the
`USE_SANITIZERS` option, with UBSan set to abort on the first finding.

```sh
cmake -B build -DLIBMEM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

Running both compilers is not redundant. Deducing-this and `if constexpr` inside
modules are exactly where GCC 15 and Clang 22 diverge, and GCC has already caught
mistakes in this suite that Clang accepted.

## What the GoogleTest suites cover

**Allocators** (`slab`, `multislab`, `arena`, `typed_arena`, `pool`): allocation,
exhaustion and reuse, slab growth and empty-slab hysteresis, full/active list
transitions, iteration, pointer stability, destructor ordering, and leak balance
via a counting `memory_resource`.

**Storage and the containers over it**: the block protocol (a reserved block leaves
the old one live, a failed growth changes nothing), relocation with a
lifetime-counting payload, alignment through `resource_ref`, the sparse/dense
invariant across growth and interleaved erase, payload/key alignment after a
swap-and-pop, move-only and non-trivially-destructible payloads, and each storage
kind per container.

**`small_storage`**: that the inline case reaches the resource zero times, that a
spill relocates the live elements and hands the block back with the size it came
with, that a failed spill leaves the inline slots exactly as they were, and that
`adopt_from` transfers the resource alongside the block. For `small_vector`, that a
spilled source moves as a pointer steal (zero element moves) and an unspilled one
moves element by element.

**The paged sparse index**: that probing an uncovered id allocates nothing, the page
boundaries at `PageSize - 1` / `PageSize` / `2 * PageSize`, that a page which could
not be allocated is never published into the directory, and a direct comparison
against the flat index over the same operations. One test measures the bytes both
actually request for twenty ids spread across a 20-million-wide range, which is the
claim the paged index exists to make.

**Concurrency**: the two-thread handoff runs for both ring variants, also under
**ThreadSanitizer** (`-DTHREAD_SANITIZER=ON`).

**Constant evaluation**: pinned with `static_assert` rather than an ordinary test,
because a runtime call passes whether or not the function can be constant-evaluated,
so only a compile-time assertion proves the claim. `inline_vector`, `sparse_set`, and
`sparse_map` each build, mutate, erase from and clone a container inside a
`consteval` lambda; `spsc_ring` is pinned with a `constinit` global instead, that
being as far as atomics allow. The non-trivial fallback is covered too: a
`std::string` element takes the byte-array branch, which cannot constant-evaluate but
must still move and clone correctly at run time.

Two helper types carry most of the weight. A counting payload proves no leaks and
no double destroys; a payload whose move throws once a budget runs out is what
actually instantiates the rollback branch of `relocate_grow`, which every
nothrow-move payload leaves discarded and therefore never even type-checked. An
auditing `memory_resource` records the exact (size, alignment) of every live block
and checks the pair it is given back, since `-fsized-deallocation` is on and a size
mismatch reaching `deallocate` is undefined behaviour that neither ASan nor
`arena`'s no-op `deallocate` would surface.

## Fuzzing

Coverage-guided **libFuzzer** harnesses drive `multislab`, `sparse_set`, and
`sparse_map` under ASan + UBSan (`-DLIBMEM_BUILD_FUZZERS=ON`, Clang only, Debug
only). CI runs a short, time-boxed smoke pass on each. The two sparse harnesses are
differential, comparing every operation against a standard container.

They currently drive the flat sparse index only. See [fuzz/README.md](../fuzz/README.md).

## Scope

This is an early-stage library and a C++26 modules testbed. Treat the above as what
is exercised today, not a guarantee of exhaustive coverage.
