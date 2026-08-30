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

## Copying and moving

One rule across every container: **never implicitly copyable, always movable.**

Copying is deleted because a copy constructor cannot report that the resource ran
out. It would have to throw `std::bad_alloc`, which is the one thing the rest of
the library avoids. Rust hits the same wall from the other side: `Clone::clone`
returns `Self`, so it cannot express failure either, and Rust's answer is to abort
on allocation failure and offer `try_reserve` for code that must cope. libmem is
already the `try_*` dialect throughout, so it reports instead of aborting.

| Operation | Where | Returns |
|-----------|-------|---------|
| `clone()` | fixed extent, where the copy always fits | the container |
| `try_clone()` | anywhere | `std::optional<container>`, empty when the storage ran out |
| `assign_range(r)` | `basic_vector` | how many elements were taken |

A clone is deep, and draws on **the same resource** the source uses rather than a
default-constructed one, which is what `resourced_storage` exists to guarantee. A
`small_vector` clone that fits inline allocates nothing.

```cpp
libmem::inline_vector<int, 8> b{a.clone()};              // cannot fail
if (auto c = heap_vec.try_clone()) { use(*c); }          // can, and says so
existing.assign_range(source);                           // clone into what you have
```

`pool` reports too: `emplace` returns an iterator equal to `end()` when the
resource could not supply a block, and `insert_range` returns how many actually
went in. Its `try_clone` rebuilds the whole allocator configuration (slab cap,
resource, policy), but holds equal elements rather than the same layout, so it may
iterate in a different order.

`pool` takes its slab cap as a named `slab_limit`, not a bare integer, because it
also has an `initializer_list` constructor:

```cpp
libmem::pool<int> capped{libmem::slab_limit{4}};   // configuration
libmem::pool<int> holding{4};                      // one element, value 4
```

A bare-integer overload would make `pool<int>{4}` mean one thing for `int` and
another for a non-integral element type, and silently drop a cap the caller thought
they had set. `std::vector<int>{4}` sets the same trap; the difference is that
`vector`'s wrong branch is visible in `size()`, while a dropped slab cap only shows
up much later as growth that was supposed to be bounded.

Every container moves, inline-backed ones included, and every move is `noexcept`.
The one exception is a `constexpr_inline_storage` over an element type whose
default constructor can throw: relocating builds the destination's slots, so that
move is `noexcept` only where building them is, and it propagates rather than
terminating. Where the storage cannot transfer its slots the move relocates the
elements and empties the source, exactly as `std::array` does. That costs O(size) rather than
O(1), which is what the container-level `relocatable` constant now reports: a cost,
not an availability. Non-movable containers were a standing tax on generic code,
so the only non-movable thing left is `spsc_ring`, deliberately, since a
synchronization primitive with two threads pointing into it should not move any
more than a `std::mutex` should.

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

Planned: `freelist`, an intrusive free-list allocator for variable-size blocks.

`find_owner` is a linear scan over slab pages, and is what makes `deallocate` O(S)
on `multislab` and `pool`. Allocation avoids it entirely: `slab::allocate_at()` and
`multislab::allocate_at()` return the block *plus* its position, both of which the
allocator already had in hand. `pool::emplace` uses this, so insertion stays
O(N/64) amortised rather than paying an O(S) scan to recover a position it just
discarded. Reach for `make_iterator(ptr)` only when you have a pointer and no
allocation to go with it.

## Constant evaluation

`inline_vector` is usable in constant expressions:

```cpp
constexpr int folded = [] {
    libmem::inline_vector<int, 8> v{};
    v.push_back(10);
    v.push_back(20);
    v.erase(v.begin());
    return v[0];
}();
static_assert(folded == 20);
```

`sparse_set` and `sparse_map` over inline storage work the same way, the whole way
through insert, erase, clear and clone:

```cpp
constexpr std::size_t n = [] {
    libmem::sparse_set<entity, libmem::inline_storage<entity, 32>> s{};
    s.insert(entity{1});
    s.insert(entity{2});
    s.erase(entity{1});
    return s.size();
}();
static_assert(n == 1);
```

`spsc_ring` stops one step short. `std::atomic` has a constexpr constructor but no
constexpr `load` or `store`, so a ring can be *built* at compile time and not
*used* there. What that buys is `constinit`, which is the part worth having for a
queue that is usually a global: no dynamic initialiser, and so no exposure to the
static-initialisation-order fiasco.

```cpp
constinit libmem::spsc_ring<command, 64> queue{};
```

All of this works for a `T` that is trivially default-constructible and trivially
destructible. `inline_storage` then holds a real
`T[N]` instead of a byte array, which is what makes it constexpr-usable, and P1331
lets that array be left trivially default-initialised inside a `constexpr`
constructor, so nothing is zeroed at runtime and `sizeof` is unchanged.
`inline_storage<T, N>::constexpr_usable` reports which branch a `T` takes.

A `T` that is `constexpr` default-constructible but not *trivially* so, which is
any type with a member initialiser or a user-provided `constexpr` constructor, is
not shut out; it has to ask. `constexpr_inline_vector<T, N>`, or
`constexpr_inline_storage<T, N>` under a sparse container, holds the same `T[N]`
and default-initialises it:

```cpp
struct vec2 { float x{}, y{}; };

constexpr float folded = [] {
    libmem::constexpr_inline_vector<vec2, 8> v{};
    v.push_back(vec2{1.0F, 2.0F});
    return v[0].x + v[0].y;
}();
static_assert(folded == 3.0F);
```

The `N` default constructions that costs, on every such container built, moved or
cloned, are why it is a separate name rather than a wider trait;
[docs/storage.md](storage.md) has the trade in full.

Raw bytes are the obstacle, not a missing keyword: `reinterpret_cast` is forbidden
during constant evaluation, so a byte array can never be viewed as a `T*`. The
single-member-union trick that constexpr `std::vector` uses does not help either,
because it only yields constexpr access by *index*: taking `&slots[0].value` and
adding one gives a one-past-the-end pointer, and constructing through it is
rejected. The `storage` concept hands out a `T*` that containers do arithmetic on,
so a real array is the only representation that satisfies both.

The other storage kinds are not constexpr-usable, for a different reason: they
reach `::operator new` through their resource, and only `std::allocator<T>::allocate`
is blessed for constant evaluation. So `vector`, `small_vector`, `fixed_vector`, and
the heap-backed sparse containers carry `constexpr` on their members but can only
ever be evaluated at run time.

One portability note. libmem does not call `std::uninitialized_move_n`,
`uninitialized_fill_n`, or `uninitialized_value_construct_n`, because libc++ has not
yet made them `constexpr` (P2283) while libstdc++ has. Using them directly would
make the inline containers constant-evaluable on GCC and not on Clang. The
equivalents in `libmem::detail` do the same work through `std::construct_at`, which
has been constexpr since C++20, and keep the same roll-back-on-throw guarantee.
