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
 *     for (auto&& [e, t] : std::views::zip(transforms.keys(), transforms.values())) { ... }
 * @endcode
 *
 * @section sparse_set_storage Storage and growth
 *
 * The storage argument picks the backing memory, exactly as it does for the ring:
 *
 *   - `dynamic_storage` (the default) grows geometrically, `std::vector`-style.
 *   - `inline_storage<Id, N>` gives a fixed set that allocates nothing, holding at
 *     most `N` ids drawn from `[0, N)` (the rebound sparse array has `N` slots
 *     too). `insert` reports failure instead of growing.
 *   - `fixed_storage<Id, N, R>` is the same fixed extent with the slots on a
 *     resource.
 *
 * @warning Growth relocates, so **every pointer, reference, and iterator into the
 *          set is invalidated by an insert that grows it**, as with
 *          `std::vector`. Note that this is the opposite of `libmem::pool`, which
 *          guarantees pointer stability; the two make different trades and a
 *          reader coming from `pool` should not assume otherwise. Erasing
 *          invalidates references to the erased element and to the last one,
 *          which is moved into its place.
 *
 * @note The sparse array is flat, not paged: it is sized by the largest id ever
 *       inserted, so memory is O(max_id) and not O(size). Ids that are dense-ish
 *       from zero (a generational entity counter, a slot index) are the intended
 *       shape. `to_index` is the hook for an id that packs extra bits it should be
 *       indexed without.
 */
module;

#include <cassert>

export module libmem:sparse_set;

import :concepts;
import :identifier;
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
 * @tparam DenseStorage Storage for the dense id array. The sparse array uses
 *                      `DenseStorage::rebind<size_type>`, so one argument
 *                      configures both, and an injected resource reaches both.
 */
export template <regular_indexable_id Id, storage_for<Id> DenseStorage = dynamic_storage<Id>> class sparse_set {
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
    using index_storage = typename DenseStorage::template rebind<size_type>;

    /** @brief Reserved index meaning "absent"; also the tombstone in the sparse array. */
    static constexpr size_type npos{std::numeric_limits<size_type>::max()};

    /** @brief Whether inserting can grow, or fails once the fixed extent is full. */
    static constexpr bool growable{growable_storage<DenseStorage> && growable_storage<index_storage>};

    /** @brief The dense extent, or `dynamic_extent` when it is a runtime property. */
    static constexpr size_type static_capacity{DenseStorage::static_capacity};

    /** @brief Whether the set can be moved; false for inline storage, which cannot relocate. */
    static constexpr bool relocatable{DenseStorage::relocatable && index_storage::relocatable};

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

    /* ========================================================================
     * Construction / destruction
     * ======================================================================== */

    sparse_set()
        requires std::default_initializable<index_storage> && std::default_initializable<DenseStorage>
    {
        fill_sparse(0);
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
        requires(sizeof...(Args) > 0) && (!std::same_as<std::remove_cvref_t<Args>, sparse_set> && ...) && std::constructible_from<index_storage, Args&...> &&
                    std::constructible_from<DenseStorage, Args&...>
    explicit sparse_set(Args&&... args) : sparse_{args...}, dense_{args...} {
        fill_sparse(0);
    }

    sparse_set(const sparse_set&) = delete;
    sparse_set& operator=(const sparse_set&) = delete;

    sparse_set(sparse_set&& other) noexcept
        requires relocatable
        : sparse_{std::move(other.sparse_)}, dense_{std::move(other.dense_)}, size_{std::exchange(other.size_, 0)} {}

    sparse_set& operator=(sparse_set&& other) noexcept
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

    ~sparse_set() { destroy_all(); }

    /* ========================================================================
     * Capacity
     * ======================================================================== */

    /** @brief Number of ids in the set. */
    constexpr size_type size() const noexcept { return size_; }

    /** @brief True when the set holds no ids. */
    constexpr bool empty() const noexcept { return size_ == 0; }

    /** @brief Ids that fit before the dense array has to grow. */
    constexpr size_type capacity() const noexcept { return dense_.capacity(); }

    /** @brief Number of id slots the sparse array currently covers, i.e. `max_id + 1`. */
    constexpr size_type index_capacity() const noexcept { return sparse_.capacity(); }

    /**
     * @brief Grow the dense array to hold at least `n` ids.
     * @return `false` when the storage could not supply the space, leaving the
     *         set untouched. Always `false` past a fixed extent.
     */
    bool reserve(const size_type n) {
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
    bool reserve_for(const Id& id) {
        const size_type at{to_index(id)};
        assert(at != npos && "sparse_set: id maps to the reserved npos subscript");

        if (at < sparse_.capacity()) {
            return true;
        }
        if constexpr (growable_storage<index_storage>) {
            const size_type covered{sparse_.capacity()};
            if (!relocate_grow(sparse_, at + 1, covered)) {
                return false;
            }
            fill_sparse(covered);
            return true;
        } else {
            return false;
        }
    }

    /* ========================================================================
     * Lookup
     * ======================================================================== */

    /** @brief Whether `id` is in the set. */
    bool contains(const Id& id) const noexcept {
        const size_type at{to_index(id)};
        return at < sparse_.capacity() && sparse_.data()[at] != npos;
    }

    /** @brief Dense position of `id`, or `npos` when it is absent. */
    size_type index_of(const Id& id) const noexcept {
        const size_type at{to_index(id)};
        return at < sparse_.capacity() ? sparse_.data()[at] : npos;
    }

    /** @brief The id at dense position `index`. */
    const Id& operator[](const size_type index) const noexcept {
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
    const_iterator begin() const noexcept { return dense_.data(); }
    const_iterator end() const noexcept { return dense_.data() + size_; }

    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    /** @brief The member ids as one contiguous view, in dense order. */
    std::span<const Id> keys() const noexcept { return {dense_.data(), size_}; }

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
    insert_result insert(const Id& id) {
        assert(!(id == null_id_v<Id>) && "sparse_set: the null id is reserved and not insertable");

        const size_type at{to_index(id)};
        assert(at != npos && "sparse_set: id maps to the reserved npos subscript");

        if (at < sparse_.capacity()) {
            if (const size_type existing{sparse_.data()[at]}; existing != npos) {
                return {existing, false};
            }
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
    size_type insert_range(R&& range) {
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
    erase_result erase(const Id& id) {
        const size_type at{to_index(id)};
        if (at >= sparse_.capacity()) {
            return {};
        }

        size_type* sparse{sparse_.data()};
        const size_type index{sparse[at]};
        if (index == npos) {
            return {};
        }

        assert(size_ > 0);

        Id* dense{dense_.data()};
        const size_type last{size_ - 1};

        if (index != last) {
            dense[index] = std::move(dense[last]);
            sparse[to_index(dense[index])] = index;
        }

        std::destroy_at(dense + last);
        sparse[at] = npos;
        --size_;

        return {true, index, index != last ? last : npos};
    }

    /**
     * @brief Remove every id, keeping the allocated capacity.
     *
     * O(size), not O(index_capacity): only the tombstones of the ids actually
     * present have to be restored.
     */
    void clear() noexcept {
        size_type* sparse{sparse_.data()};
        const Id* dense{dense_.data()};

        for (size_type i{}; i < size_; ++i) {
            sparse[to_index(dense[i])] = npos;
        }

        std::destroy_n(dense_.data(), size_);
        size_ = 0;
    }

    /* ========================================================================
     * Observers
     * ======================================================================== */

    /** @brief Access the dense array's storage. */
    constexpr auto& storage(this auto&& self) noexcept { return self.dense_; }

private:
    index_storage sparse_;
    DenseStorage dense_;
    size_type size_{};

    /**
     * @brief Tombstone every sparse slot from `from` to the current capacity.
     *
     * Keeps the invariant that all `sparse_.capacity()` slots hold a live
     * `size_type`, so a lookup never reads uninitialised memory and growth never
     * has to track a separate initialised count.
     */
    void fill_sparse(const size_type from) noexcept {
        size_type* sparse{sparse_.data()};
        for (size_type i{from}; i < sparse_.capacity(); ++i) {
            std::construct_at(sparse + i, npos);
        }
    }

    /**
     * @brief Append `id` at the dense back and point sparse slot `at` at it.
     * @pre Both arrays already have room; see `reserve` and `reserve_for`.
     * @return The new dense position.
     */
    size_type push_back_reserved(const Id& id, const size_type at) {
        /* Construct first: if the copy throws, neither the sparse slot nor the
         * size has been touched yet. */
        std::construct_at(dense_.data() + size_, id);
        sparse_.data()[at] = size_;
        return size_++;
    }

    void destroy_all() noexcept {
        std::destroy_n(dense_.data(), size_);
        std::destroy_n(sparse_.data(), sparse_.capacity());
        size_ = 0;
    }
};

/* ============================================================================
 * sparse_map: a payload per id
 * ============================================================================ */

/**
 * @brief Id-keyed map holding one `T` per member id, dense-packed alongside the keys.
 *
 * Composed of a `sparse_set` plus a parallel payload array, which is what keeps
 * `keys()` and `values()` both contiguous and index-aligned: the pair is what an
 * ECS-style consumer wants to iterate, and either can be handed to an algorithm
 * on its own.
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
 *
 * @warning Growth invalidates every pointer, reference, and iterator, and erase
 *          invalidates those to the erased element and to the last one. See
 *          @ref sparse_set_storage.
 */
export template <regular_indexable_id Id, typename T, storage_for<Id> DenseStorage = dynamic_storage<Id>>
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

    using key_set = sparse_set<Id, DenseStorage>;
    using value_storage = typename DenseStorage::template rebind<T>;

    static constexpr size_type npos{key_set::npos};
    static constexpr bool growable{key_set::growable && growable_storage<value_storage>};
    static constexpr size_type static_capacity{key_set::static_capacity};
    static constexpr bool relocatable{key_set::relocatable && value_storage::relocatable};

    /* ========================================================================
     * Construction / destruction
     * ======================================================================== */

    sparse_map()
        requires std::default_initializable<key_set> && std::default_initializable<value_storage>
    = default;

    /**
     * @brief Construct the key set and the payload array from `args`, typically a resource.
     * @note `args` are passed as lvalues, not forwarded: each array needs its own copy.
     */
    template <typename... Args>
        requires(sizeof...(Args) > 0) && (!std::same_as<std::remove_cvref_t<Args>, sparse_map> && ...) && std::constructible_from<key_set, Args&...> &&
                    std::constructible_from<value_storage, Args&...>
    explicit sparse_map(Args&&... args) : keys_{args...}, values_{args...} {}

    sparse_map(const sparse_map&) = delete;
    sparse_map& operator=(const sparse_map&) = delete;

    sparse_map(sparse_map&& other) noexcept
        requires relocatable
        : keys_{std::move(other.keys_)}, values_{std::move(other.values_)} {}

    sparse_map& operator=(sparse_map&& other) noexcept
        requires relocatable
    {
        if (this != &other) {
            clear();
            keys_ = std::move(other.keys_);
            values_ = std::move(other.values_);
        }
        return *this;
    }

    ~sparse_map() { clear(); }

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
    bool reserve(const size_type n) {
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
    bool contains(const Id& id) const noexcept { return keys_.contains(id); }

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
    std::span<const Id> keys() const noexcept { return keys_.keys(); }

    /** @brief The payloads, in the same order as `keys()`; zip the two to walk pairs. */
    constexpr auto values(this auto&& self) noexcept { return std::span{self.values_.data(), self.keys_.size()}; }

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
    std::pair<T*, bool> emplace(const Id& id, Args&&... args) {
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

    /** @brief Insert `value` for `id`, or overwrite the payload if `id` already has one. */
    template <typename V>
        requires std::constructible_from<T, V&&> && std::assignable_from<T&, V&&>
    std::pair<T*, bool> insert_or_assign(const Id& id, V&& value) {
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
    bool erase(const Id& id) {
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
    void clear() noexcept {
        std::destroy_n(values_.data(), keys_.size());
        keys_.clear();
    }

private:
    key_set keys_;
    value_storage values_;
};

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

/* A fixed inline extent neither grows nor moves: its slots are the object. */
static_assert(!inline_set::growable && !inline_set::relocatable);
static_assert(!std::movable<inline_set>);
static_assert(inline_set::static_capacity == 64);

static_assert(std::ranges::contiguous_range<sparse_map<sparse_test_id, int>::key_set>);
static_assert(std::movable<sparse_map<sparse_test_id, std::size_t>>);

} // namespace detail

} // namespace libmem
