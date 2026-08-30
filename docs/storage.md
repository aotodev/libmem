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
| `constexpr_inline_storage<T, N>` | compile-time | inside the object, default-initialised |

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

## `inline_storage` and constant evaluation

`inline_storage` holds its slots two different ways, chosen on the element type:

| `T` | Representation | Constant evaluation |
|-----|----------------|---------------------|
| trivially default-constructible **and** trivially destructible | a real `T[N]` | yes |
| anything else | `std::byte[N * sizeof(T)]` | no |

`inline_storage<T, N>::constexpr_usable` reports which branch a `T` took. Standard
library implementations of `std::inplace_vector` face the same obstacle and solve it
the same way, so the set of element types that constant-evaluate is comparable.

Raw bytes are the obstacle, not a missing keyword. `reinterpret_cast` is forbidden
during constant evaluation, so a byte array can never be viewed as a `T*`. Nor does
the single-member-union trick that constexpr `std::vector` uses help here: it yields
constexpr access by *index* only, because taking `&slots[0].value` and adding one
produces a one-past-the-end pointer and constructing through it is rejected. The
`storage` concept hands out a `T*` that containers do arithmetic on, so a genuine
array is the only representation satisfying both.

Neither `sizeof` nor `alignof` changes, and nothing is zeroed. The array carries no
initialiser, and P1331 permits leaving it trivially default-initialised inside a
`constexpr` constructor, so an `inline_vector<int, 1024>` still costs nothing to
construct at run time.

The resource-backed kinds cannot follow, for an unrelated reason: they reach
`::operator new` through their resource, and only `std::allocator<T>::allocate` is
blessed for constant evaluation. Their members carry `constexpr` but can only ever
run at run time.

## Opting in: `constexpr_inline_storage`

That table is a property of `T`, not a choice, and it is stricter than constant
evaluation itself needs. A `T` whose default constructor is `constexpr` but not
trivial (an aggregate with member initialisers, a user-provided `constexpr`
constructor) can hold a real `T[N]` perfectly well. What it cannot hold is an
*uninitialised* one, which is the only thing the trait is protecting.

`constexpr_inline_storage<T, N>` makes that trade, named at the call site:

```cpp
struct vec2 { float x{}, y{}; };                     // not trivially default constructible

libmem::inline_vector<vec2, 8> plain{};              // raw bytes, run time only
libmem::constexpr_inline_vector<vec2, 8> folded{};   // vec2[8], constant-evaluable
```

The cost is `N` default constructions every time a storage is built, for slots
nobody has used yet, which is the work `inline_storage` exists to avoid. Every
storage object pays it, so a move and a clone each pay it again over the
destination's slots, independently of how many elements are live. Hence a separate
name rather than a relaxed trait: worth paying when it is asked for, not worth
paying silently. Capacity, alignment and `sizeof` are unchanged, and for a
trivially default constructible `T` the two are the same thing in all but name.

`T` must still be trivially destructible. The slot array destroys its elements, so
a real destructor would run a second time over what the container already
destroyed. `rebind<U>` carries both requirements, which is what a `sparse_map` over
this storage applies to its mapped type.

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
- `resource_allocator<T, R>` is the mirror image, and the one to reach for when the
  container is theirs rather than ours. See [interop](#standard-container-interop).
- Over-aligned storage needs an `aligned_memory_resource` (`allocate(size, align)`
  plus the matching sized `deallocate`).

The aligned and unaligned halves are not interchangeable. Memory taken from
`allocate(size, align)` must be released through `deallocate(ptr, size, align)`
with the same alignment, which is why `allocate_slots` and `free_slots` branch on
one condition and must never diverge.

### Monotonic resources

A caller that carves a fixed set of blocks once and never frees them
individually, letting the resource reclaim everything together, needs more than
`memory_resource` promises. `default_resource` satisfies the interface and would
leak every block under that usage.

`monotonic_resource` (and `aligned_monotonic_resource`) express the stronger
requirement: `deallocate` is a no-op and the memory is released wholesale, by
`reset()` or by destruction. `arena` and `typed_arena` model it; `resource_ref<R>`
inherits whatever `R` is.

The promise is semantic, so it is an opt-in trait rather than a `requires`
clause; a no-op `deallocate` is indistinguishable from a real one by signature.
A caller's own bump allocator joins with one line:

```cpp
template <>
inline constexpr bool libmem::enable_monotonic_resource<my_bump> = true;
```

Requiring `reset()` syntactically instead would be no better: it would reject
`resource_ref` to an arena, which forwards allocation but not reset.

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

Containers use `adopt_from` where it exists and fall back to relocating their
elements where it does not, so every one of them moves regardless of what its
storage can do. See [containers.md](containers.md#copying-and-moving).

A third concept, `resourced_storage`, says a storage exposes the resource its slots
came from and can be rebuilt against it. That is what lets a container clone itself
into the same arena instead of a default-constructed resource. Getting it wrong is
quiet rather than loud: the clone would hold a null `resource_ref` and assert at its
first allocation, far from the call that made the mistake.

One `sizeof` surprise worth knowing: `rebind<U>` keeps `N` as a slot count, not a
byte budget. A `sparse_set<entity, small_storage<entity, 64>>` gets 64 inline dense
slots **and** 64 inline `size_type` sparse slots.

## Standard container interop

The two adapters are mirrors of each other, and the names are easy to swap by
accident:

| Adapter | Direction | Use when |
|---------|-----------|----------|
| `allocator_resource<Alloc>` | Allocator -> resource | a `std::allocator` should back a libmem container |
| `resource_allocator<T, R>` | resource -> Allocator | a libmem `arena` should back a `std::vector` |

```cpp
libmem::arena scratch{1 << 20};
using alloc = libmem::resource_allocator<int, libmem::resource_ref<libmem::arena>>;

std::vector<int, alloc> v{alloc{libmem::resource_ref{scratch}}};
std::list<int, alloc>   l{alloc{libmem::resource_ref{scratch}}};   // rebinds to its node type
```

`resource_allocator` is the **one place in libmem where allocation failure is
reported by throwing**. `Allocator::allocate` is required to return valid memory or
throw, so a null return is not expressible; the adapter raises `std::bad_alloc`, and
`std::bad_array_new_length` when the element count would overflow. Everything on the
libmem side of the boundary still reports by value. Rust puts its own OOM handling in
the same place, at the allocator boundary, and for the same reason.

Two things to know:

**A stateful resource must go through `resource_ref`.** Standard containers copy
their allocator freely, and how often is unspecified, so a resource held by value is
copied with it and every copy gets private state. The implementations really do
differ: a by-value budget of one serves a thousand `std::vector` reallocations under
libc++ and runs out under libstdc++. A stateless resource such as `default_resource`
is unaffected, which is why it is the default.

**All three `propagate_on_container_*` traits are `true_type`,** so a container
carries the source's allocator across copy-assign, move-assign, and swap. That is
what keeps those operations defined when two allocators reference different arenas;
swapping containers with unequal non-propagating allocators is undefined behaviour.
Equality follows the resource where the resource is comparable, and answers `true`
for a stateless one, since an empty type has no state to differ on.
