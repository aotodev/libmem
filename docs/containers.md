# Containers

Every container here takes its buffer as a [storage](storage.md) argument. What
follows is what each one adds on top.

## Sequences

`basic_vector<Storage>` is contiguous and growable. The four aliases differ only in
which storage they name:

```cpp
libmem::vector<node> nodes{};                        // heap, geometric growth
libmem::small_vector<node, 8> few{};                 // 8 inline, spills past that
libmem::inline_vector<node, 8> bounded{};            // 8 inline, never grows
libmem::fixed_vector<node, 4096> off_object{};       // fixed extent, slots on a resource
```

Failure stays a return value rather than an exception, matching the resources
underneath. `push_back` and `emplace_back` return a `T*` that is null when the
storage could not make room, `reserve` and `resize` return `bool`, and there is no
throwing `at()`. On a fixed extent that same null simply means full, which is what
lets the bounded and growable variants be one class. `T`'s own constructors may of
course throw, and when one does the vector is left exactly as it was.

Two members beyond the usual surface:

- `erase_unordered(pos)` moves the last element into the hole. O(1) instead of
  O(size - index), at the cost of the order. The same trade `sparse_set::erase`
  makes.
- `truncate(n)` shrinks only, and so requires nothing of `T`. `resize` needs a
  default constructor even when it is shortening the vector, which is a
  `std::vector` wart not worth inheriting.

There is deliberately no mid-sequence `insert` and no `shrink_to_fit`. Unspilling a
`small_storage` would need a shrink step in the block protocol first.

A vector moves when its storage is `relocatable` (a pointer steal) or
`transferable_storage` (`small_storage`, via `adopt_from`). `inline_vector` is
neither and does not move, exactly as `spsc_ring` and `sparse_set` over inline
storage do not. `small_vector` additionally needs a default-constructible resource,
since the destination storage has to exist before it can adopt anything.

## Identifiers

`sparse_set` and `sparse_map` key on `regular_indexable_id`: an unsigned integer, a
scoped enum over one, or a strong-id class with an unsigned conversion operator, so
a caller does not have to give up type safety to get O(1) lookup.

```cpp
enum class entity : std::uint32_t {};
static_assert(libmem::regular_indexable_id<entity>);
```

`null_id_v<Id>` is the reserved sentinel. For integral and enum ids it is the
maximum representable value, which is therefore not a usable id: the addressable
range is `[0, max())`. A class id opts out by declaring its own static `null_id`.

The `to_index` CPO maps an id to its subscript, preferring a member `to_index()`,
then ADL `to_index(id)`, then the built-in conversion. An id that packs a
generation counter or a type tag into its bits will want to mask that off before
being used as a subscript, and those hooks are where. Without one, the whole value
is the index and the sparse array is sized by it.

## Sparse set and map

A sparse set pairs two arrays. The *sparse* array is indexed by the id itself and
holds, for a member id, that id's position in the *dense* array; the dense array
holds the member ids back to back with no gaps. Membership is one bounds check and
one load, erase is a swap with the last dense element, and iteration walks
contiguous memory.

`sparse_map` is that plus a parallel payload array, which is what keeps `keys()`
and `values()` both contiguous and index-aligned.

Growth relocates, so every pointer, reference, and iterator is invalidated by an
insert that grows the container, as with `std::vector`. This is the opposite of
`libmem::pool`, which guarantees pointer stability; the two make different trades
and a reader coming from `pool` should not assume otherwise.

### The sparse side: flat or paged

Only the dense array is an ordinary buffer. The sparse one is addressed by id and
never iterated, which is what lets it be a third template argument with two
implementations:

| Index | Lookup | Memory |
|-------|--------|--------|
| `flat_sparse_index` (default) | one load | O(max_id) |
| `paged_sparse_index<PageSize, R>` | two loads | O(size + max_id / PageSize) |

```cpp
libmem::sparse_set<entity> dense{};                  // ids clustered near zero
libmem::paged_sparse_set<entity> scattered{};        // ids spread over a wide range
libmem::paged_sparse_map<entity, transform> t{};     // same for the map
```

The flat array is sized by the largest id ever inserted, so one far-away id sizes
the whole thing. It is the right shape whenever ids are dense-ish from zero, which
is what a generational entity counter or a slot index gives you.

The paged one splits the subscript range into pages (512 entries by default, a
4 KiB page of 64-bit subscripts) and allocates a page only when an id lands in it.
An absent page answers "not a member" from the directory load alone, without
allocating, which is what keeps probing a far-away id cheap.

Note what that is and is not. It is a large constant factor, not a change of
complexity class: the directory is still indexed by id, so memory stays linear in
`max_id`, divided by the page size. A genuinely O(size) sparse side would have to be
a hash map, which is a different container with a different lookup cost, and the
point of a sparse set is that lookup stays two loads and no hashing.

### Ranges

`sparse_set` is a contiguous range of ids. `sparse_map` is a random-access range of
`(id, payload)` tuples over its two parallel arrays, so the standard adaptors apply
to it directly. `basic_vector` is a contiguous range. All have `std::from_range_t`
constructors, so `std::ranges::to` builds them.

```cpp
for (int& hp : health | std::views::values) { hp -= 1; }         // writes through
auto ids   = health | std::views::keys | std::ranges::to<std::vector>();
auto alive = ids | std::views::filter(is_alive) | std::ranges::to<libmem::sparse_set<entity>>();
auto m     = std::views::zip(ids, values) | std::ranges::to<libmem::sparse_map<entity, int>>();
auto v     = ids | std::ranges::to<libmem::small_vector<entity, 8>>();
```

`keys()` and `values()` remain available as contiguous spans when that is what an
algorithm wants.

One standard wrinkle worth knowing: `std::ranges::to` will not move elements out of
an rvalue container, because a container's reference type stays an lvalue reference
regardless of how it is passed. Reach for `std::views::as_rvalue` when the payload
is move-only.

## Allocators

| Component | Description |
|-----------|-------------|
| `slab` | Fixed-size block allocator with compile-time bitmap tracking. |
| `multislab` | Auto-expanding chain of slabs with hysteresis-based shrink policy. |
| `arena` | Bump/region allocator for trivially-destructible types. |
| `typed_arena` | Bump allocator with LIFO destructor chain for arbitrary types. |
| `pool` | Pointer-stable typed container (bitmap-based object pool) over `multislab`. |

`find_owner` is a linear scan over slab pages, and is what makes `deallocate` O(S)
on `multislab` and `pool`. Allocation avoids it entirely: `slab::allocate_at()` and
`multislab::allocate_at()` return the block *plus* its position, both of which the
allocator already had in hand. `pool::emplace` uses this, so insertion stays
O(N/64) amortised rather than paying an O(S) scan to recover a position it just
discarded. Reach for `make_iterator(ptr)` only when you have a pointer and no
allocation to go with it.
