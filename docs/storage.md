# Storage

Containers do not hard-code their buffer. Each takes a `storage` argument that owns
raw space for its elements and nothing else; the container manages the element
lifetimes itself. Splitting memory from lifetime is what lets one container serve
an inline buffer, a resource-backed fixed buffer, and a growing heap buffer without
touching its own logic.

| Storage | Extent | Slots |
|---------|--------|-------|
| `inline_storage<T, N>` | compile-time | inside the object, no allocation |
| `fixed_storage<T, N, R>` | compile-time | from a `memory_resource` |
| `dynamic_storage<T, R>` | runtime, geometric growth | from a `memory_resource` |
| `small_storage<T, N, R>` | runtime, geometric growth | inside the object for the first `N`, then from a `memory_resource` |

```cpp
libmem::spsc_ring<command, 64> small{};              // slots inline
libmem::heap_spsc_ring<command, 1 << 20> big{};      // same ring, slots on the heap

libmem::arena scratch{1 << 20};
libmem::sparse_set<entity, libmem::dynamic_storage<entity, libmem::resource_ref<libmem::arena>>>
    ids{libmem::resource_ref{scratch}};              // grows out of the arena

libmem::small_vector<hit, 8> hits{};                 // allocates nothing until the 9th hit
```

`rebind<U>` yields the same storage kind holding `U`, keeping any injected resource
and resetting the alignment to `alignof(U)`. That is what lets a container hold
several parallel arrays of different element types over one storage template
argument, as `std::allocator_traits::rebind_alloc` does for allocators.

## Resources

`memory_resource` is the single allocation interface: `allocate(size)` and
`deallocate(ptr, size)`. Failure is a null return, never an exception.

- `resource_ref<R>` shares one resource between containers that each store theirs
  by value. Without it, handing three containers the same `arena` would give each
  its own copy.
- `allocator_resource<Alloc>` adapts any standard Allocator, so a
  `std::pmr::polymorphic_allocator` drops straight in. It is deliberately *not* an
  `aligned_memory_resource`: the Allocator interface cannot express a runtime
  alignment, so over-aligned storage refuses to instantiate against it rather than
  quietly under-aligning.
- Over-aligned storage needs an `aligned_memory_resource` (`allocate(size, align)`
  plus the matching sized `deallocate`).

The aligned and unaligned halves are not interchangeable. Memory taken from
`allocate(size, align)` must be released through `deallocate(ptr, size, align)`
with the same alignment, which is why `allocate_slots` and `free_slots` branch on
one condition and must never diverge.

## The growth protocol

Growth is three steps rather than one `try_grow`, because only the container knows
which slots hold live objects and how to move them:

1. `reserve_block(n)` allocates a fresh block. The current block stays live.
2. The container relocates its live elements into the new block.
3. `adopt(block)` releases the old block and takes the new one, or
   `discard(block)` throws the new one away and keeps the old.

The payoff is that a failure at any point leaves the container exactly as it was,
including when a move constructor throws halfway through step 2, and that a
container growing two parallel arrays can order the fallible steps ahead of the
irreversible ones. That is how `sparse_map::emplace` stays exception-safe across a
key array and a payload array. `relocate_grow` wraps the single-array case.

A block reports the capacity actually allocated, which may exceed what was asked
for: geometric growth happens inside `reserve_block`, and the sized `deallocate` on
the way back out has to be given the real figure.

## `small_storage` and the two movability questions

`small_storage` is the small-buffer optimisation expressed as a storage, so any
container over it gets one for free. `capacity()` starts at `N` with `data()`
pointing into the object; the first growth past `N` takes a heap block and never
comes back. Nothing moves back into the inline slots when the element count drops,
because the storage does not know how many elements are live and a container that
shrank below `N` would otherwise pay a relocation for it.

That creates a problem the other three kinds do not have. `relocatable` is a
compile-time constant, but whether a `small_storage`'s slots can be transferred is
a runtime property: yes once spilled, no while inline. Three ways out, two of them
sound:

- **`relocatable = false`.** Honest and free, but then no container over it moves,
  which for a vector is close to fatal: it could not be returned from a function or
  held inside another container.
- **`relocatable = true` with a conditional pointer steal.** Unsound. The inline
  case would move the size without moving the elements.
- **Ask at runtime.** What `llvm::SmallVector`, `boost::container::small_vector`
  and `absl::InlinedVector` all do, and what libmem does.

So `relocatable` stays `false`, and a second concept carries the runtime question:

```cpp
template <typename S>
concept transferable_storage = growable_storage<S> && requires(S& s, S& other) {
    { s.adopt_from(other) } -> std::same_as<bool>;
};
```

`adopt_from` steals a spilled block outright and reports `false` when the source is
still inline, leaving the caller to move the elements into its own inline slots,
which they are guaranteed to fit. It takes the source's **resource** either way,
spilled or not. That detail is what makes the transfer sound between two storages
built on different arenas: a block is never released through a resource that did
not supply it, and a destination that fell back to the element-wise path still
allocates from wherever its source did.

The upshot is that `sparse_set` and `sparse_map` over `small_storage` correctly
refuse to move, since they only ever move by moving their storage, while
`basic_vector` opts into the runtime path and `small_vector` stays movable.

One `sizeof` surprise worth knowing: `rebind<U>` keeps `N` as a slot count, not a
byte budget. A `sparse_set<entity, small_storage<entity, 64>>` gets 64 inline dense
slots **and** 64 inline `size_type` sparse slots.
