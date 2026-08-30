/**
 * @file sparse_set.cppm
 * @brief Dense-packed id set and id-keyed map with O(1) insert, erase, and lookup.
 *
 * A sparse set pairs two arrays. The *sparse* array is indexed by the id itself
 * and holds, for a member id, the position of that id in the *dense* array; the
 * dense array holds the member ids back to back with no gaps. Membership is one
 * bounds check and one load, erase is a swap with the last dense element, and
 * iteration walks contiguous memory.
 *
 * @code
 *     enum class entity : std::uint32_t {};
 *
 *     libmem::sparse_set<entity> alive{};
 *     alive.insert(entity{7});
 *     if (alive.contains(entity{7})) { ... }
 *     for (const entity e : alive) { ... }              // contiguous, no gaps
 *
 *     libmem::sparse_map<entity, transform> transforms{};
 *     transforms.emplace(entity{7}, position, rotation);
 *     if (auto* t = transforms.find(entity{7})) { ... }
 *     for (auto&& [e, t] : transforms) { ... }           // a range of (id, payload)
 * @endcode
 *
 * @section sparse_set_ranges Ranges
 *
 * `sparse_set` is a contiguous range of ids. `sparse_map` is a random-access range
 * of two-element tuples over its two parallel arrays, so the standard adaptors
 * apply to it directly:
 *
 * @code
 *     for (int& hp : health | std::views::values) { hp -= 1; }
 *     auto ids = health | std::views::keys | std::ranges::to<std::vector>();
 * @endcode
 *
 * Both have `std::from_range_t` constructors, so `std::ranges::to` can build them:
 *
 * @code
 *     auto alive = ids | std::views::filter(is_alive) | std::ranges::to<libmem::sparse_set<entity>>();
 *     auto m     = std::views::zip(ids, values) | std::ranges::to<libmem::sparse_map<entity, int>>();
 * @endcode
 *
 * `keys()` and `values()` remain available when a plain contiguous span of one side
 * is what an algorithm wants; `each()` names the zipped view that `begin()` and
 * `end()` are built on.
 *
 * @section sparse_set_storage Storage and growth
 *
 * The storage argument picks the backing memory, exactly as it does for the ring:
 *
 *   - `dynamic_storage` (the default) grows geometrically, `std::vector`-style.
 *   - `inline_storage<Id, N>` gives a fixed set that allocates nothing, holding at
 *     most `N` ids drawn from `[0, N)` (the rebound sparse array has `N` slots
 *     too). `insert` reports failure instead of growing.
 *   - `constexpr_inline_storage<Id, N>` is that same fixed set with the slots
 *     default-initialised, which is what keeps a map constant-evaluable over a
 *     mapped type that is not trivially default constructible.
 *   - `fixed_storage<Id, N, R>` is the same fixed extent with the slots on a
 *     resource.
 *
 * @warning Growth relocates, so **every pointer, reference, and iterator into the
 *          set is invalidated by an insert that grows it**, as with
 *          `std::vector`, and the opposite of `libmem::pool`. Erasing invalidates
 *          references to the erased element and to the last one, which is moved
 *          into its place.
 *
 * @section sparse_set_index The sparse side
 *
 * A third template argument, defaulting to `flat_sparse_index`: one subscript per
 * id in `[0, max_id]`, so memory is O(max_id) and not O(size). `to_index` is the
 * hook for an id that packs extra bits it should be indexed without.
 *
 * `paged_sparse_set` swaps in `paged_sparse_index`, which allocates page-sized
 * spans of subscripts on demand. See @ref sparse_index_cost.
 *
 * @code
 *     libmem::sparse_set<entity> dense{};              // ids clustered near zero
 *     libmem::paged_sparse_set<entity> scattered{};    // ids spread over a wide range
 * @endcode
 */
module;

#include <cassert>

export module libmem:sparse_set;

import :concepts;
import :identifier;
import :sparse_index;
import :storage;
import std;

namespace libmem {

/* ============================================================================
 * sparse_set: membership only
 * ============================================================================ */

/**
 * @brief Dense-packed set of ids with O(1) insert, erase, and membership.
 *
 * @tparam Id           Key type; see `regular_indexable_id`. `null_id_v<Id>` is
 *                      the reserved sentinel and is not insertable.
 * @tparam DenseStorage Storage for the dense id array.
 * @tparam SparseIndex  Map from id subscript to dense position; see @ref
 *                      sparse_set_index. Defaults to a flat array over
 *                      `DenseStorage::rebind<size_type>`, so one storage argument
 *                      configures both arrays and an injected resource reaches both.
 */
export template <regular_indexable_id Id, storage_for<Id> DenseStorage = dynamic_storage<Id>,
    sparse_index SparseIndex = flat_sparse_index<typename DenseStorage::template rebind<std::size_t>>>
class sparse_set {
public:
    /* ========================================================================
     * Member types
     * ======================================================================== */

    using value_type = Id;
    using reference = const Id&;
    using const_reference = const Id&;
    using pointer = const Id*;
    using const_pointer = const Id*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using iterator = const Id*;
    using const_iterator = const Id*;

    using dense_storage = DenseStorage;
    using index_type = SparseIndex;

    /** @brief Reserved index meaning "absent"; also the tombstone in the sparse array. */
    static constexpr size_type npos{sparse_npos};

    /** @brief Whether inserting can grow, or fails once the fixed extent is full. */
    static constexpr bool growable{growable_storage<DenseStorage> && SparseIndex::growable};

    /** @brief The dense extent, or `dynamic_extent` when it is a runtime property. */
    static constexpr size_type static_capacity{DenseStorage::static_capacity};

    /**
     * @brief Whether a move transfers the buffers instead of relocating the ids.
     *
     * A cost, not an availability: every set moves. `false` for inline storage,
     * whose move copies the ids across and tombstones the source.
     */
    static constexpr bool relocatable{DenseStorage::relocatable && SparseIndex::relocatable};

    /** @brief Outcome of `insert`: the dense position, and whether it was newly added. */
    struct insert_result {
        size_type index{npos};
        bool inserted{};

        /** @brief True when the id is in the set, whether or not this call put it there. */
        constexpr explicit operator bool() const noexcept { return index != npos; }
    };

    /**
     * @brief Outcome of `erase`, carrying enough to fix up a parallel array.
     *
     * `index` is the dense slot the erased id occupied. `moved_from` is the slot
     * whose element was swapped into it, or `npos` when the erased id was already
     * last. `sparse_map` uses exactly this to mirror the swap onto its payload.
     */
    struct erase_result {
        bool erased{};
        size_type index{npos};
        size_type moved_from{npos};

        constexpr explicit operator bool() const noexcept { return erased; }
    };

private:
    /* Private type, public constructor: only members can spell the tag, while
     * `std::optional::emplace` can still forward it. */
    struct copy_of_t {
        explicit copy_of_t() = default;
    };

public:
    /* ========================================================================
     * Construction / destruction
     * ======================================================================== */

    constexpr sparse_set()
        requires std::default_initializable<SparseIndex> && std::default_initializable<DenseStorage>
    = default;

    /**
     * @brief Build both arrays from `args` and insert every id of `source`.
     *
     * The body of `clone` and `try_clone`. Inserts what fits, so `try_clone` can
     * compare sizes afterwards.
     */
    template <typename... Args>
        requires std::constructible_from<SparseIndex, Args&...> && std::constructible_from<DenseStorage, Args&...>
    constexpr sparse_set(copy_of_t, const sparse_set& source, Args&&... args) : sparse_{args...}, dense_{args...} {
        static_cast<void>(insert_range(source));
    }

    /**
     * @brief Construct both arrays from `args`, typically a resource.
     *
     * @code
     *     libmem::arena scratch{1 << 20};
     *     libmem::sparse_set<entity, libmem::dynamic_storage<entity, libmem::resource_ref<libmem::arena>>>
     *         ids{libmem::resource_ref{scratch}};
     * @endcode
     *
     * @note `args` are passed as lvalues, not forwarded: there are two arrays to
     *       build and each needs its own copy.
     */
    template <typename... Args>
        requires(sizeof...(Args) > 0) && (!std::same_as<std::remove_cvref_t<Args>, sparse_set> && ...) && std::constructible_from<SparseIndex, Args&...> &&
                    std::constructible_from<DenseStorage, Args&...>
    constexpr explicit sparse_set(Args&&... args) : sparse_{args...}, dense_{args...} {}

    /**
     * @brief Construct from a range of ids, so `std::ranges::to` can build one.
     *
     * @code
     *     auto alive = ids | std::views::filter(is_alive) | std::ranges::to<libmem::sparse_set<entity>>();
     * @endcode
     */
    template <std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, const Id&> && std::default_initializable<SparseIndex> &&
                 std::default_initializable<DenseStorage>
    constexpr sparse_set(std::from_range_t, R&& range) : sparse_set() {
        insert_range(std::forward<R>(range));
    }

    sparse_set(const sparse_set&) = delete;
    sparse_set& operator=(const sparse_set&) = delete;

    /** @brief Storage whose slots travel: both arrays come across as pointers. */
    constexpr sparse_set(sparse_set&& other) noexcept
        requires relocatable
        : sparse_{std::move(other.sparse_)}, dense_{std::move(other.dense_)}, size_{std::exchange(other.size_, 0)} {}

    /**
     * @brief Inline-backed set: the sparse index drains and the dense ids are relocated one at a time.
     *
     * @note Requires a non-throwing id move, which leaves no half-moved state to
     *       unwind: the sparse index is drained in the mem-initialiser, before the
     *       dense ids are touched.
     * @note The dense slots are default-constructed here, so the move is `noexcept`
     *       only where that construction is; see `constexpr_inline_storage`, the
     *       one storage for which it may not be.
     */
    constexpr sparse_set(sparse_set&& other) noexcept(std::is_nothrow_default_constructible_v<DenseStorage>)
        requires(!relocatable) && std::default_initializable<SparseIndex> && std::default_initializable<DenseStorage> &&
                std::is_nothrow_move_constructible_v<Id>
        : sparse_{std::move(other.sparse_)} {
        adopt_dense(other);
    }

    constexpr sparse_set& operator=(sparse_set&& other) noexcept
        requires relocatable
    {
        if (this != &other) {
            destroy_all();
            sparse_ = std::move(other.sparse_);
            dense_ = std::move(other.dense_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    constexpr sparse_set& operator=(sparse_set&& other) noexcept
        requires(!relocatable) && std::is_nothrow_move_constructible_v<Id>
    {
        if (this != &other) {
            destroy_all();
            sparse_ = std::move(other.sparse_);
            adopt_dense(other);
        }
        return *this;
    }

    constexpr ~sparse_set() { destroy_all(); }

    /* ========================================================================
     * Capacity
     * ======================================================================== */

    /** @brief Number of ids in the set. */
    constexpr size_type size() const noexcept { return size_; }

    /** @brief True when the set holds no ids. */
    constexpr bool empty() const noexcept { return size_ == 0; }

    /** @brief Ids that fit before the dense array has to grow. */
    constexpr size_type capacity() const noexcept { return dense_.capacity(); }

    /** @brief Number of id subscripts the sparse index currently covers, i.e. `max_id + 1`. */
    constexpr size_type index_capacity() const noexcept { return sparse_.covered(); }

    /**
     * @brief Grow the dense array to hold at least `n` ids.
     * @return `false` when the storage could not supply the space, leaving the
     *         set untouched. Always `false` past a fixed extent.
     */
    constexpr bool reserve(const size_type n) {
        if (dense_.capacity() >= n) {
            return true;
        }
        if constexpr (growable_storage<DenseStorage>) {
            return relocate_grow(dense_, n, size_);
        } else {
            return false;
        }
    }

    /**
     * @brief Grow the sparse array to cover `id`.
     *
     * Separate from `reserve` because the two dimensions grow for unrelated
     * reasons: the dense array by element count, the sparse array by largest id.
     *
     * @return `false` when the storage could not supply the space, leaving the
     *         set untouched.
     */
    constexpr bool reserve_for(const Id& id) {
        const size_type at{to_index(id)};
        assert(at != npos && "sparse_set: id maps to the reserved npos subscript");

        return sparse_.reserve_for(at);
    }

    /* ========================================================================
     * Lookup
     * ======================================================================== */

    /** @brief Whether `id` is in the set. */
    constexpr bool contains(const Id& id) const noexcept { return sparse_.get(to_index(id)) != npos; }

    /** @brief Dense position of `id`, or `npos` when it is absent. */
    constexpr size_type index_of(const Id& id) const noexcept { return sparse_.get(to_index(id)); }

    /** @brief The id at dense position `index`. */
    constexpr const Id& operator[](const size_type index) const noexcept {
        assert(index < size_ && "sparse_set: dense index out of range");
        return dense_.data()[index];
    }

    /* ========================================================================
     * Iteration: the dense array is contiguous, so plain pointers suffice
     * ======================================================================== */

    /**
     * @brief Iterator to the first id.
     *
     * Const throughout: rewriting an id in place would desynchronise it from the
     * sparse array that points at it.
     */
    constexpr const_iterator begin() const noexcept { return dense_.data(); }
    constexpr const_iterator end() const noexcept { return dense_.data() + size_; }

    constexpr const_iterator cbegin() const noexcept { return begin(); }
    constexpr const_iterator cend() const noexcept { return end(); }

    /** @brief The member ids as one contiguous view, in dense order. */
    constexpr std::span<const Id> keys() const noexcept { return {dense_.data(), size_}; }

    /* ========================================================================
     * Modifiers
     * ======================================================================== */

    /**
     * @brief Add `id` to the set.
     *
     * @return `{dense position, true}` when newly added, `{existing position,
     *         false}` when already present, and a falsy `{npos, false}` when the
     *         storage could not make room.
     * @pre `id != null_id_v<Id>`.
     */
    constexpr insert_result insert(const Id& id) {
        assert(!(id == null_id_v<Id>) && "sparse_set: the null id is reserved and not insertable");

        const size_type at{to_index(id)};
        assert(at != npos && "sparse_set: id maps to the reserved npos subscript");

        if (const size_type existing{sparse_.get(at)}; existing != npos) {
            return {existing, false};
        }

        if (!reserve_for(id) || !reserve(size_ + 1)) {
            return {};
        }

        return {push_back_reserved(id, at), true};
    }

    /**
     * @brief Insert every id of `range`.
     * @return How many were newly added.
     */
    template <std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, const Id&>
    constexpr size_type insert_range(R&& range) {
        size_type added{};
        for (const Id& id : range) {
            if (insert(id).inserted) {
                ++added;
            }
        }
        return added;
    }

    /**
     * @brief Remove `id`, swapping the last dense element into its place.
     * @return A falsy result when `id` was not in the set.
     */
    constexpr erase_result erase(const Id& id) {
        const size_type at{to_index(id)};
        const size_type index{sparse_.get(at)};
        if (index == npos) {
            return {};
        }

        assert(size_ > 0);

        Id* dense{dense_.data()};
        const size_type last{size_ - 1};

        if (index != last) {
            dense[index] = std::move(dense[last]);
            sparse_.set(to_index(dense[index]), index);
        }

        std::destroy_at(dense + last);
        sparse_.set(at, npos);
        --size_;

        return {true, index, index != last ? last : npos};
    }

    /**
     * @brief Remove every id, keeping the allocated capacity.
     *
     * O(size), not O(index_capacity): only the tombstones of the ids actually
     * present have to be restored.
     */
    constexpr void clear() noexcept {
        const Id* dense{dense_.data()};

        for (size_type i{}; i < size_; ++i) {
            sparse_.set(to_index(dense[i]), npos);
        }

        std::destroy_n(dense_.data(), size_);
        size_ = 0;
    }

    /* ========================================================================
     * Copying: explicit, never implicit
     * ======================================================================== */

    /**
     * @brief A deep copy, drawing on the same resource this set uses.
     *
     * The fixed-extent counterpart to `try_clone`: both sets have the same static
     * extent, so every id fits and there is nothing to report.
     */
    constexpr sparse_set clone() const
        requires fixed_extent_storage<DenseStorage> && std::default_initializable<SparseIndex> && std::default_initializable<DenseStorage>
    {
        if constexpr (resourced_storage<DenseStorage>) {
            return sparse_set{copy_of_t{}, *this, dense_.resource()};
        } else {
            return sparse_set{copy_of_t{}, *this};
        }
    }

    /**
     * @brief A deep copy, drawing on the same resource this set uses.
     *
     * Copying is never implicit: a copy constructor cannot report that the storage
     * ran out, so it would have to throw.
     *
     * @return `std::nullopt` when the storage could not take every id.
     */
    constexpr std::optional<sparse_set> try_clone() const
        requires std::default_initializable<SparseIndex> && std::default_initializable<DenseStorage>
    {
        std::optional<sparse_set> copy{};

        if constexpr (resourced_storage<DenseStorage>) {
            copy.emplace(copy_of_t{}, *this, dense_.resource());
        } else {
            copy.emplace(copy_of_t{}, *this);
        }

        if (copy->size() != size_) {
            return std::nullopt;
        }
        return copy;
    }

    /* ========================================================================
     * Observers
     * ======================================================================== */

    /** @brief Access the dense array's storage. */
    constexpr auto& storage(this auto&& self) noexcept { return self.dense_; }

    /** @brief Access the sparse index, e.g. to ask a paged one how many pages it holds. */
    constexpr auto& sparse(this auto&& self) noexcept { return self.sparse_; }

private:
    SparseIndex sparse_;
    DenseStorage dense_;
    size_type size_{};

    /**
     * @brief Append `id` at the dense back and point sparse subscript `at` at it.
     * @pre Both arrays already have room; see `reserve` and `reserve_for`.
     * @return The new dense position.
     */
    constexpr size_type push_back_reserved(const Id& id, const size_type at) {
        /* Construct first: if the copy throws, neither the sparse slot nor the
         * size has been touched yet. */
        std::construct_at(dense_.data() + size_, id);
        sparse_.set(at, size_);
        return size_++;
    }

    /* The sparse index owns its own slot lifetimes, so there is nothing to clean
     * up here but the dense elements. */
    constexpr void destroy_all() noexcept {
        std::destroy_n(dense_.data(), size_);
        size_ = 0;
    }

    /**
     * @brief Relocate `other`'s dense ids into these slots and empty it.
     * @pre This dense array holds no live ids, and `other.sparse_` has already been drained.
     */
    constexpr void adopt_dense(sparse_set& other) noexcept {
        assert(other.size_ <= dense_.capacity() && "sparse_set: source does not fit the destination slots");

        detail::relocate_n(other.dense_.data(), other.size_, dense_.data());
        size_ = std::exchange(other.size_, 0);
        std::destroy_n(other.dense_.data(), size_);
    }
};

/* ============================================================================
 * sparse_map: a payload per id
 * ============================================================================ */

namespace detail {

/**
 * @brief A range of two-element tuple-likes usable as `(id, payload)` entries.
 *
 * Spelled through `std::get` rather than a structured binding so the value
 * category of the element carries through: an rvalue range moves its payloads in,
 * an lvalue range copies them. Accepts `std::pair`, `std::tuple`, and whatever
 * `std::views::zip` yields.
 */
template <typename R, typename Id, typename T>
concept pair_range_for = std::ranges::input_range<R> && requires(std::ranges::range_reference_t<R> entry) {
    requires std::tuple_size_v<std::remove_cvref_t<std::ranges::range_reference_t<R>>> == 2;
    { std::get<0>(entry) } -> std::convertible_to<const Id&>;
    requires std::constructible_from<T, decltype(std::get<1>(std::forward<std::ranges::range_reference_t<R>>(entry)))>;
};

} // namespace detail

/**
 * @brief Id-keyed map holding one `T` per member id, dense-packed alongside the keys.
 *
 * A `sparse_set` plus a parallel payload array, so `keys()` and `values()` are
 * both contiguous and index-aligned.
 *
 * @tparam Id           Key type; see `regular_indexable_id`.
 * @tparam T            Payload type. Any object type: lifetimes are managed
 *                      explicitly, so a `std::string` or `std::vector` payload
 *                      works and is destroyed on erase, clear, and destruction.
 *                      `erase` move-**assigns** the last payload into the hole, so
 *                      it additionally needs `T` to be move-assignable; a payload
 *                      that is not compiles until the first `erase`. This is the
 *                      same requirement `std::vector::erase` imposes.
 * @tparam DenseStorage Storage for the dense key array; the payload array uses
 *                      `DenseStorage::rebind<T>`.
 * @tparam SparseIndex  Map from id subscript to dense position; see @ref
 *                      sparse_set_index.
 *
 * @warning Growth invalidates every pointer, reference, and iterator, and erase
 *          invalidates those to the erased element and to the last one. See
 *          @ref sparse_set_storage.
 */
export template <regular_indexable_id Id, typename T, storage_for<Id> DenseStorage = dynamic_storage<Id>,
    sparse_index SparseIndex = flat_sparse_index<typename DenseStorage::template rebind<std::size_t>>>
    requires std::is_object_v<T>
class sparse_map {
public:
    /* ========================================================================
     * Member types
     * ======================================================================== */

    using key_type = Id;
    using mapped_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    using key_set = sparse_set<Id, DenseStorage, SparseIndex>;
    using value_storage = typename DenseStorage::template rebind<T>;

    static constexpr size_type npos{key_set::npos};
    static constexpr bool growable{key_set::growable && growable_storage<value_storage>};
    static constexpr size_type static_capacity{key_set::static_capacity};

    /** @brief Whether a move transfers the buffers instead of relocating the entries; see `sparse_set::relocatable`. */
    static constexpr bool relocatable{key_set::relocatable && value_storage::relocatable};

private:
    /* Private type, public constructor; see `sparse_set::copy_of_t`. */
    struct copy_of_t {
        explicit copy_of_t() = default;
    };

public:
    /* ========================================================================
     * Construction / destruction
     * ======================================================================== */

    constexpr sparse_map()
        requires std::default_initializable<key_set> && std::default_initializable<value_storage>
    = default;

    /**
     * @brief Build both arrays from `args` and emplace every entry of `source`.
     *
     * The body of `clone` and `try_clone`. Emplaces what fits, so `try_clone` can
     * compare sizes afterwards.
     */
    template <typename... Args>
        requires std::constructible_from<key_set, Args&...> && std::constructible_from<value_storage, Args&...> && std::copy_constructible<T>
    constexpr sparse_map(copy_of_t, const sparse_map& source, Args&&... args) : keys_{args...}, values_{args...} {
        for (const auto& [id, value] : source) {
            if (!emplace(id, value).first) {
                break;
            }
        }
    }

    /**
     * @brief Construct the key set and the payload array from `args`, typically a resource.
     * @note `args` are passed as lvalues, not forwarded: each array needs its own copy.
     */
    template <typename... Args>
        requires(sizeof...(Args) > 0) && (!std::same_as<std::remove_cvref_t<Args>, sparse_map> && ...) && std::constructible_from<key_set, Args&...> &&
                    std::constructible_from<value_storage, Args&...>
    constexpr explicit sparse_map(Args&&... args) : keys_{args...}, values_{args...} {}

    /**
     * @brief Construct from a range of `(id, payload)` pairs, so `std::ranges::to` can build one.
     *
     * @code
     *     auto m = std::views::zip(ids, values) | std::ranges::to<libmem::sparse_map<entity, transform>>();
     * @endcode
     */
    template <std::ranges::input_range R>
        requires detail::pair_range_for<R, Id, T> && std::default_initializable<key_set> && std::default_initializable<value_storage>
    constexpr sparse_map(std::from_range_t, R&& range) : sparse_map() {
        insert_range(std::forward<R>(range));
    }

    sparse_map(const sparse_map&) = delete;
    sparse_map& operator=(const sparse_map&) = delete;

    /** @brief Storage whose slots travel: both arrays come across as pointers. */
    constexpr sparse_map(sparse_map&& other) noexcept
        requires relocatable
        : keys_{std::move(other.keys_)}, values_{std::move(other.values_)} {}

    /**
     * @brief Inline-backed map: the payloads are relocated one at a time.
     *
     * @note Requires a non-throwing payload move, so nothing is left half-moved.
     *       The payloads are relocated *before* the key set is moved, so
     *       `other.size()` still describes them while they travel.
     * @note Both arrays are default-constructed here, so the move is `noexcept`
     *       only where those constructions are; see `constexpr_inline_storage`, the
     *       one storage for which they may not be.
     */
    constexpr sparse_map(sparse_map&& other) noexcept(
        std::is_nothrow_default_constructible_v<key_set> && std::is_nothrow_default_constructible_v<value_storage>)
        requires(!relocatable) && std::default_initializable<key_set> && std::default_initializable<value_storage> && std::is_nothrow_move_constructible_v<T> &&
                std::movable<key_set>
    {
        adopt_values(other);
        keys_ = std::move(other.keys_);
    }

    constexpr sparse_map& operator=(sparse_map&& other) noexcept
        requires relocatable
    {
        if (this != &other) {
            clear();
            keys_ = std::move(other.keys_);
            values_ = std::move(other.values_);
        }
        return *this;
    }

    constexpr sparse_map& operator=(sparse_map&& other) noexcept
        requires(!relocatable) && std::is_nothrow_move_constructible_v<T> && std::movable<key_set>
    {
        if (this != &other) {
            clear();
            adopt_values(other);
            keys_ = std::move(other.keys_);
        }
        return *this;
    }

    constexpr ~sparse_map() { clear(); }

    /* ========================================================================
     * Capacity
     * ======================================================================== */

    constexpr size_type size() const noexcept { return keys_.size(); }
    constexpr bool empty() const noexcept { return keys_.empty(); }
    constexpr size_type capacity() const noexcept { return keys_.capacity(); }
    constexpr size_type index_capacity() const noexcept { return keys_.index_capacity(); }

    /**
     * @brief Grow both arrays to hold at least `n` entries.
     *
     * The payload grows first, because it is the array whose relocation can throw
     * and because a payload that grew while the key array did not is merely
     * over-allocated, not inconsistent.
     *
     * @return `false` when either storage could not supply the space.
     */
    constexpr bool reserve(const size_type n) {
        if (values_.capacity() < n) {
            if constexpr (growable_storage<value_storage>) {
                if (!relocate_grow(values_, n, keys_.size())) {
                    return false;
                }
            } else {
                return false;
            }
        }
        return keys_.reserve(n);
    }

    /* ========================================================================
     * Lookup
     * ======================================================================== */

    /** @brief Whether `id` has an entry. */
    constexpr bool contains(const Id& id) const noexcept { return keys_.contains(id); }

    /** @brief Pointer to the payload of `id`, or `nullptr` when absent. */
    constexpr auto* find(this auto&& self, const Id& id) noexcept {
        const size_type at{self.keys_.index_of(id)};
        return at == npos ? nullptr : self.values_.data() + at;
    }

    /**
     * @brief The payload of `id`.
     * @pre `contains(id)`.
     */
    constexpr auto& at(this auto&& self, const Id& id) noexcept {
        auto* found{self.find(id)};
        assert(found != nullptr && "sparse_map: no entry for this id");
        return *found;
    }

    /* ========================================================================
     * Iteration: two contiguous, index-aligned views
     * ======================================================================== */

    /** @brief The member ids, in dense order. */
    constexpr std::span<const Id> keys() const noexcept { return keys_.keys(); }

    /** @brief The payloads, in the same order as `keys()`. */
    constexpr auto values(this auto&& self) noexcept { return std::span{self.values_.data(), self.keys_.size()}; }

    /**
     * @brief The entries as a range of `(id, payload)` pairs.
     *
     * A `std::views::zip` over the two spans. `begin()` and `end()` forward to it,
     * which is what makes the map itself a random-access range.
     *
     * @note Both spans are borrowed ranges, so the returned iterators outlive this
     *       temporary view.
     */
    constexpr auto each(this auto&& self) noexcept { return std::views::zip(self.keys(), self.values()); }

    /** @brief Iterator over `(id, payload)` pairs, in dense order. */
    constexpr auto begin(this auto&& self) noexcept { return self.each().begin(); }
    constexpr auto end(this auto&& self) noexcept { return self.each().end(); }

    /** @brief The key set itself, for membership queries and dense-index lookups. */
    constexpr const key_set& key_set_view() const noexcept { return keys_; }

    /* ========================================================================
     * Modifiers
     * ======================================================================== */

    /**
     * @brief Construct a payload for `id` in place if it has no entry yet.
     *
     * @return `{pointer to the payload, true}` when newly inserted, `{pointer to
     *         the existing payload, false}` when `id` was already present, and
     *         `{nullptr, false}` when the storage could not make room.
     * @throws Whatever `T`'s constructor throws, having changed nothing.
     * @pre `id != null_id_v<Id>`.
     */
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    constexpr std::pair<T*, bool> emplace(const Id& id, Args&&... args) {
        if (const size_type existing{keys_.index_of(id)}; existing != npos) {
            return {values_.data() + existing, false};
        }

        /* Every fallible step first, so the irreversible ones cannot be reached
         * from a state that has to be unwound. */
        if (!reserve(keys_.size() + 1) || !keys_.reserve_for(id)) {
            return {nullptr, false};
        }

        const size_type at{keys_.size()};
        T* slot{values_.data() + at};

        /* The only step that can throw. Nothing is published yet, so an exception
         * propagates with the map exactly as it was. */
        std::construct_at(slot, std::forward<Args>(args)...);

        if constexpr (std::is_nothrow_copy_constructible_v<Id>) {
            keys_.insert(id);
        } else {
            try {
                keys_.insert(id);
            } catch (...) {
                std::destroy_at(slot);
                throw;
            }
        }

        assert(keys_.size() == at + 1 && "sparse_map: key insert did not land where the payload did");
        return {slot, true};
    }

    /**
     * @brief Emplace every `(id, payload)` entry of `range`.
     *
     * Entries whose id is already present keep their existing payload, matching
     * `emplace` rather than `insert_or_assign`.
     *
     * @return How many entries were newly inserted.
     */
    template <std::ranges::input_range R>
        requires detail::pair_range_for<R, Id, T>
    constexpr size_type insert_range(R&& range) {
        size_type added{};
        for (auto&& entry : range) {
            if (emplace(std::get<0>(entry), std::get<1>(std::forward<decltype(entry)>(entry))).second) {
                ++added;
            }
        }
        return added;
    }

    /** @brief Insert `value` for `id`, or overwrite the payload if `id` already has one. */
    template <typename V>
        requires std::constructible_from<T, V&&> && std::assignable_from<T&, V&&>
    constexpr std::pair<T*, bool> insert_or_assign(const Id& id, V&& value) {
        if (const size_type existing{keys_.index_of(id)}; existing != npos) {
            T* slot{values_.data() + existing};
            *slot = std::forward<V>(value);
            return {slot, false};
        }
        return emplace(id, std::forward<V>(value));
    }

    /**
     * @brief Remove the entry for `id`, swapping the last one into its place.
     * @return `false` when `id` had no entry.
     */
    constexpr bool erase(const Id& id) {
        const auto removed{keys_.erase(id)};
        if (!removed) {
            return false;
        }

        /* keys_ has already done its swap-and-pop and decremented; mirror it.
         * `moved_from == npos` means the erased entry was the last one. */
        T* values{values_.data()};
        const size_type last{removed.moved_from == npos ? removed.index : removed.moved_from};

        if (last != removed.index) {
            values[removed.index] = std::move(values[last]);
        }
        std::destroy_at(values + last);

        return true;
    }

    /** @brief Destroy every payload and empty the key set, keeping the capacity. */
    constexpr void clear() noexcept {
        std::destroy_n(values_.data(), keys_.size());
        keys_.clear();
    }

    /* ========================================================================
     * Copying: explicit, never implicit
     * ======================================================================== */

    /**
     * @brief A deep copy, drawing on the same resource this map uses.
     *
     * The fixed-extent counterpart to `try_clone`: both maps have the same static
     * extent, so every entry fits and there is nothing to report.
     */
    constexpr sparse_map clone() const
        requires fixed_extent_storage<DenseStorage> && std::default_initializable<key_set> && std::default_initializable<value_storage> &&
                 std::copy_constructible<T>
    {
        if constexpr (resourced_storage<value_storage>) {
            return sparse_map{copy_of_t{}, *this, values_.resource()};
        } else {
            return sparse_map{copy_of_t{}, *this};
        }
    }

    /**
     * @brief A deep copy, drawing on the same resource this map uses.
     *
     * @return `std::nullopt` when the storage could not take every entry.
     * @throws Whatever `T`'s copy constructor throws.
     */
    constexpr std::optional<sparse_map> try_clone() const
        requires std::default_initializable<key_set> && std::default_initializable<value_storage> && std::copy_constructible<T>
    {
        std::optional<sparse_map> copy{};

        if constexpr (resourced_storage<value_storage>) {
            copy.emplace(copy_of_t{}, *this, values_.resource());
        } else {
            copy.emplace(copy_of_t{}, *this);
        }

        if (copy->size() != size()) {
            return std::nullopt;
        }
        return copy;
    }

private:
    key_set keys_;
    value_storage values_;

    /**
     * @brief Relocate `other`'s payloads into these slots and destroy the originals.
     * @pre These payload slots hold nothing live, and `other.keys_` has not been moved yet.
     */
    constexpr void adopt_values(sparse_map& other) noexcept {
        const size_type count{other.keys_.size()};
        assert(count <= values_.capacity() && "sparse_map: source does not fit the destination slots");

        detail::relocate_n(other.values_.data(), count, values_.data());
        std::destroy_n(other.values_.data(), count);
    }
};

/* ============================================================================
 * Paged aliases
 * ============================================================================ */

/**
 * @brief `sparse_set` whose sparse side allocates pages on demand.
 *
 * For ids spread thinly over a wide range. Same interface and same O(1)
 * operations, one extra indirection per lookup. See @ref sparse_index_cost.
 */
export template <regular_indexable_id Id, memory_resource Resource = default_resource>
using paged_sparse_set = sparse_set<Id, dynamic_storage<Id, Resource>, paged_sparse_index<default_sparse_page_size, Resource>>;

/** @brief `sparse_map` whose sparse side allocates pages on demand; see `paged_sparse_set`. */
export template <regular_indexable_id Id, typename T, memory_resource Resource = default_resource>
using paged_sparse_map = sparse_map<Id, T, dynamic_storage<Id, Resource>, paged_sparse_index<default_sparse_page_size, Resource>>;

/* ============================================================================
 * Concept verification
 * ============================================================================ */

namespace detail {

enum class sparse_test_id : std::uint32_t {
};

using dynamic_set = sparse_set<sparse_test_id>;
using inline_set = sparse_set<sparse_test_id, inline_storage<sparse_test_id, 64>>;

static_assert(std::ranges::contiguous_range<dynamic_set>);
static_assert(std::contiguous_iterator<dynamic_set::iterator>);
static_assert(dynamic_set::growable && dynamic_set::relocatable);
static_assert(std::movable<dynamic_set>);

/* A fixed inline extent does not grow, and its move relocates the ids rather than
 * transferring a buffer, but it does move. */
static_assert(!inline_set::growable && !inline_set::relocatable);
static_assert(std::movable<inline_set>);
static_assert(std::is_nothrow_move_constructible_v<inline_set>);
static_assert(inline_set::static_capacity == 64);

static_assert(std::ranges::contiguous_range<sparse_map<sparse_test_id, int>::key_set>);
static_assert(std::movable<sparse_map<sparse_test_id, std::size_t>>);

/* sparse_map zips two parallel arrays, so it is random-access but never
 * contiguous: the (id, payload) pairs do not exist in memory as pairs. Being a
 * range of two-element tuples is what lets std::views::keys / values / elements
 * apply to the map directly. */
using dynamic_map = sparse_map<sparse_test_id, int>;

static_assert(std::ranges::random_access_range<dynamic_map>);
static_assert(std::ranges::random_access_range<const dynamic_map>);
static_assert(std::ranges::common_range<dynamic_map>);
static_assert(!std::ranges::contiguous_range<dynamic_map>);
static_assert(std::tuple_size_v<std::ranges::range_value_t<dynamic_map>> == 2);
static_assert(std::same_as<std::tuple_element_t<1, std::ranges::range_reference_t<dynamic_map>>, int&>);
static_assert(std::same_as<std::tuple_element_t<1, std::ranges::range_reference_t<const dynamic_map>>, const int&>);

/* Both are buildable by std::ranges::to via their from_range_t constructors. */
static_assert(std::constructible_from<dynamic_set, std::from_range_t, std::span<const sparse_test_id>>);
static_assert(std::constructible_from<dynamic_map, std::from_range_t, std::span<const std::pair<sparse_test_id, int>>>);

/* The paged variants differ only in the sparse side. */
using paged_set = paged_sparse_set<sparse_test_id>;
using paged_map = paged_sparse_map<sparse_test_id, int>;

static_assert(std::ranges::contiguous_range<paged_set>);
static_assert(paged_set::growable && paged_set::relocatable);
static_assert(std::movable<paged_set>);
static_assert(std::ranges::random_access_range<paged_map>);
static_assert(std::movable<paged_map>);
static_assert(std::constructible_from<paged_set, std::from_range_t, std::span<const sparse_test_id>>);

/* A small-buffer set grows, and moves by relocating rather than transferring. */
using small_set = sparse_set<sparse_test_id, small_storage<sparse_test_id, 32>>;

static_assert(small_set::growable);
static_assert(!small_set::relocatable);
static_assert(std::movable<small_set>);

/* Copying is never implicit; a deep copy goes through clone / try_clone. */
static_assert(!std::copyable<dynamic_set>);
static_assert(!std::copyable<inline_set>);

/* An inline-backed set and map work during constant evaluation, the whole way
 * through insert, erase, clear and clone. */
consteval std::size_t constexpr_set() {
    inline_set s{};
    for (std::uint32_t i{}; i < 10; ++i) {
        s.insert(sparse_test_id{i});
    }
    s.erase(sparse_test_id{3});
    s.erase(sparse_test_id{7});

    const inline_set copy{s.clone()};
    return copy.size() + (copy.contains(sparse_test_id{3}) ? 100 : 0);
}

using inline_map = sparse_map<sparse_test_id, int, inline_storage<sparse_test_id, 32>>;

consteval int constexpr_map() {
    inline_map m{};
    m.emplace(sparse_test_id{1}, 10);
    m.emplace(sparse_test_id{2}, 20);
    m.erase(sparse_test_id{1});

    const int* found{m.find(sparse_test_id{2})};
    return found ? *found : -1;
}

/* A payload with member initialisers is not trivially default constructible, so
 * the dense storage has to be asked for typed slots for the map to stay
 * constant-evaluable over it. */
struct sparse_test_value {
    float x{}, y{};
};

using constexpr_inline_map = sparse_map<sparse_test_id, sparse_test_value, constexpr_inline_storage<sparse_test_id, 32>>;

consteval float constexpr_default_init_map() {
    constexpr_inline_map m{};
    m.emplace(sparse_test_id{1}, sparse_test_value{1.0F, 2.0F});
    m.emplace(sparse_test_id{2}, sparse_test_value{3.0F, 4.0F});
    m.erase(sparse_test_id{1});

    const sparse_test_value* found{m.find(sparse_test_id{2})};
    return found ? found->x + found->y : -1.0F;
}

static_assert(constexpr_set() == 8);
static_assert(constexpr_map() == 20);
static_assert(constexpr_default_init_map() == 7.0F);

/* The dense slots and the payloads are default-constructed by the move, so a map
 * over the opt-in storage keeps its `noexcept` move only while those cannot throw. */
static_assert(std::is_nothrow_move_constructible_v<constexpr_inline_map>);
static_assert(std::is_nothrow_move_constructible_v<inline_map>);

template <typename S>
concept has_clone = requires(const S& s) { s.clone(); };

template <typename S>
concept has_try_clone = requires(const S& s) { s.try_clone(); };

/* clone only where the copy cannot report a failure; try_clone everywhere. */
static_assert(has_clone<inline_set>);
static_assert(!has_clone<dynamic_set>);
static_assert(!has_clone<small_set>);
static_assert(has_try_clone<dynamic_set> && has_try_clone<inline_set> && has_try_clone<paged_set>);
static_assert(has_try_clone<dynamic_map> && has_try_clone<paged_map>);
static_assert(std::same_as<decltype(std::declval<const dynamic_set&>().try_clone()), std::optional<dynamic_set>>);

} // namespace detail

} // namespace libmem
