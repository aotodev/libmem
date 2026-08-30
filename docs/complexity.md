# Complexity

## Allocators

| Operation | `slab` | `multislab` | `arena` / `typed_arena` | `pool` |
|-----------|--------|-------------|-------------------------|--------|
| allocate | O(N/64) | O(N/64) amortised | O(1) | O(N/64) amortised |
| deallocate | O(1) | O(S) | no-op | O(S) |
| iterate (skip empties) | O(N/64) per word | O(N/64) per word | n/a | O(N/64) per word |
| reset / clear | O(W) | O(S) | O(1) / O(D) | O(S + E) |

N = capacity in blocks, W = bitmap words (N/64), S = number of slab pages,
D = registered destructors, E = live elements (for non-trivial destructors).

The O(S) terms are all `find_owner`, a linear scan over slab pages. See
[containers.md](containers.md#allocators) for why allocation does not pay it.

## Sequences

`basic_vector` is the ordinary `std::vector` profile.

| Operation | Cost |
|-----------|------|
| subscript / `back` | O(1) |
| `push_back` / `emplace_back` | O(1) amortised |
| `pop_back` / `truncate` | O(1) plus the destructors |
| `erase(pos)` | O(size - index) |
| `erase_unordered(pos)` | O(1) |
| `clear` | O(size) |

`small_vector` adds nothing to any of these. It just does not allocate at all until
it outgrows its inline slots.

## Copying and moving

| Operation | Cost |
|-----------|------|
| move, `relocatable` storage | O(1), a pointer steal |
| move, otherwise | O(size) element relocations, no allocation |
| `basic_vector::clone` / `try_clone` | O(size) copies, one allocation when growable |
| `sparse_set` / `sparse_map` clone | O(size) copies, one allocation per array |
| `pool::try_clone` | O(size) inserts, one slab page per `BlocksPerSlab` elements |

A clone is never cheaper than the container it copies, which is why it is spelled
out rather than reachable by an implicit copy. `pool::try_clone` is the one that
also rearranges: slots come from the allocator, so a clone of a pool with erased
holes is packed differently and may iterate in a different order.

## Index-addressed containers

| Operation | `sparse_set` / `sparse_map` |
|-----------|-----------------------------|
| contains / find | O(1), one bounds check and one load (two loads with a paged index) |
| insert | O(1) amortised |
| erase | O(1), swap with the last dense element |
| iterate | O(size), contiguous and gap-free |
| clear | O(size), not O(max_id) |

Memory is O(max_id) with the default flat index and O(size + max_id / PageSize)
with a paged one.

The trade against `pool` is stability: `pool` never relocates a live element, while
these relocate on growth exactly as `std::vector` does. Iterating is the mirror
image, contiguous here versus a bitmap scan there.

## Constant evaluation

Free, in the sense that matters: `inline_storage` holds a real `T[N]` where the
element type allows it, so `sizeof` and `alignof` are identical on both branches and
the array is never zeroed. A container that constant-evaluates costs exactly what
the same container costs at run time.

`constexpr_inline_storage` is the one exception, and the reason it is opt-in rather
than a wider trait: it default-initialises its slots, so construction runs `N`
default constructors where `inline_storage` runs none. `sizeof` and `alignof` are
still identical, and every other operation still costs what it costs at run time.

See [containers.md](containers.md#constant-evaluation) for which containers qualify.
