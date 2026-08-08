/**
 * @file sparse_index.cppm
 * @brief The sparse half of a sparse set: id subscript -> dense position, flat or paged.
 *
 * Two implementations of the id-subscript-to-dense-position map:
 *
 *   - `flat_sparse_index<S>`: one array covering `[0, max_id]`. One load per
 *     lookup, and `max_id + 1` subscripts allocated whether or not they are used.
 *   - `paged_sparse_index<PageSize, R>`: a directory of fixed-size pages,
 *     allocated only for the id ranges actually touched.
 *
 * @section sparse_index_cost Cost
 *
 * Flat is `O(max_id)` regardless of how few ids are in the set. Paged is
 * `O(size + max_id / PageSize)`: still linear in the largest id, since the
 * directory is indexed by id, but divided by the page size and with pages
 * allocated only where ids landed.
 *
 * @code
 *     libmem::sparse_set<entity> dense{};              // flat, ids clustered near zero
 *     libmem::paged_sparse_set<entity> scattered{};    // paged, ids spread thin
 * @endcode
 */
module;

#include <cassert>

export module libmem:sparse_index;

import :concepts;
import :storage;
import std;

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)

namespace libmem {

/**
 * @brief Reserved subscript meaning "absent"; the tombstone every sparse index writes.
 *
 * `sparse_set::npos` is this value. It is spelled once here so the container and
 * its index cannot disagree about what an empty slot looks like.
 */
export inline constexpr std::size_t sparse_npos{std::numeric_limits<std::size_t>::max()};

/**
 * @brief Map from an id subscript to a dense position, or `sparse_npos` when absent.
 *
 * Not a `storage`: there is no `data()`, a paged implementation having no single
 * contiguous array. An index owns its own slot lifetimes; the container does not
 * run `destroy_n` over them.
 *
 * @pre `get` must be safe and non-allocating for **any** subscript, covered or not.
 */
export template <typename M>
concept sparse_index = requires(M& m, const M& cm, std::size_t at, std::size_t index) {
    typename M::size_type;

    { M::npos } -> std::convertible_to<std::size_t>;
    /* Whether `reserve_for` can extend the covered range, or only reports failure. */
    { M::growable } -> std::convertible_to<bool>;
    /* Whether moving the index transfers its slots; false for an inline-backed one. */
    { M::relocatable } -> std::convertible_to<bool>;

    /* Subscripts currently covered, i.e. one past the largest that `set` accepts. */
    { cm.covered() } -> std::same_as<std::size_t>;
    { cm.get(at) } -> std::same_as<std::size_t>;
    { m.set(at, index) } -> std::same_as<void>;
    { m.reserve_for(at) } -> std::same_as<bool>;
};

/* ============================================================================
 * flat_sparse_index: one array over [0, max_id]
 * ============================================================================ */

/**
 * @brief Sparse index holding one subscript per id in a single array.
 *
 * The default. A lookup is one bounds check and one load; the array is sized by
 * the largest id ever inserted.
 *
 * @tparam Storage Storage for the subscripts. Whatever the dense array uses,
 *                 rebound to `size_type`, so one storage argument on `sparse_set`
 *                 configures both arrays and an injected resource reaches both.
 */
export template <typename Storage>
    requires storage_for<Storage, std::size_t>
class flat_sparse_index {
public:
    using size_type = std::size_t;
    using storage_type = Storage;

    static constexpr size_type npos{sparse_npos};
    static constexpr bool growable{growable_storage<Storage>};
    static constexpr bool relocatable{Storage::relocatable};

    flat_sparse_index()
        requires std::default_initializable<Storage>
    {
        fill_from(0);
    }

    /** @brief Construct the underlying storage from `args`, typically a resource. */
    template <typename... Args>
        requires(sizeof...(Args) > 0) && (!std::same_as<std::remove_cvref_t<Args>, flat_sparse_index> && ...) && std::constructible_from<Storage, Args...>
    explicit flat_sparse_index(Args&&... args) : slots_{std::forward<Args>(args)...} {
        fill_from(0);
    }

    flat_sparse_index(const flat_sparse_index&) = delete;
    flat_sparse_index& operator=(const flat_sparse_index&) = delete;

    flat_sparse_index(flat_sparse_index&& other) noexcept
        requires relocatable
        : slots_{std::move(other.slots_)} {}

    flat_sparse_index& operator=(flat_sparse_index&& other) noexcept
        requires relocatable
    {
        if (this != &other) {
            release();
            slots_ = std::move(other.slots_);
        }
        return *this;
    }

    ~flat_sparse_index() { release(); }

    constexpr size_type covered() const noexcept { return slots_.capacity(); }

    size_type get(const size_type at) const noexcept { return at < slots_.capacity() ? slots_.data()[at] : npos; }

    void set(const size_type at, const size_type index) noexcept {
        assert(at < slots_.capacity() && "flat_sparse_index: subscript not reserved");
        slots_.data()[at] = index;
    }

    /**
     * @brief Extend the covered range to include `at`.
     * @return `false` when the storage could not supply the space, leaving the
     *         index untouched. Always `false` past a fixed extent.
     */
    bool reserve_for(const size_type at) {
        assert(at != npos && "flat_sparse_index: npos is the reserved tombstone, not a subscript");

        if (at < slots_.capacity()) {
            return true;
        }
        if constexpr (growable) {
            const size_type previous{slots_.capacity()};
            if (!relocate_grow(slots_, at + 1, previous)) {
                return false;
            }
            fill_from(previous);
            return true;
        } else {
            return false;
        }
    }

    /** @brief Access the underlying storage. */
    constexpr auto& storage(this auto&& self) noexcept { return self.slots_; }

private:
    Storage slots_;

    /**
     * @brief Tombstone every slot from `from` to the current capacity.
     *
     * Keeps the invariant that all `covered()` slots hold a live `size_type`, so a
     * lookup never reads uninitialised memory and growth never has to track a
     * separate initialised count.
     */
    void fill_from(const size_type from) noexcept {
        size_type* slots{slots_.data()};
        for (size_type i{from}; i < slots_.capacity(); ++i) {
            std::construct_at(slots + i, npos);
        }
    }

    void release() noexcept { std::destroy_n(slots_.data(), slots_.capacity()); }
};

/* ============================================================================
 * paged_sparse_index: a directory of lazily allocated pages
 * ============================================================================ */

/** @brief Subscripts per page, chosen so a page of 64-bit subscripts is 4 KiB. */
export inline constexpr std::size_t default_sparse_page_size{4096 / sizeof(std::size_t)};

/**
 * @brief Sparse index splitting the subscript range into fixed-size pages, allocated on demand.
 *
 * A page is allocated only when an id lands in it. A lookup is one indirection
 * more than the flat index: bounds check, load the page pointer, load the
 * subscript. An absent page answers `npos` from the pointer load, without
 * allocating. See @ref sparse_index_cost.
 *
 * @tparam PageSize Subscripts per page; a power of two, so the division and
 *                  remainder are a shift and a mask.
 * @tparam Resource Resource supplying both the directory and the pages.
 *
 * @note Pages are never released short of destruction; `sparse_set::clear` keeps
 *       them, as it keeps the dense array's capacity.
 */
export template <std::size_t PageSize = default_sparse_page_size, memory_resource Resource = default_resource>
    requires power_of_two<PageSize>
class paged_sparse_index {
public:
    using size_type = std::size_t;
    using resource_type = Resource;
    using directory_storage = dynamic_storage<size_type*, Resource>;

    static constexpr size_type npos{sparse_npos};
    static constexpr size_type page_size{PageSize};
    static constexpr bool growable{true};
    static constexpr bool relocatable{true};

    paged_sparse_index()
        requires std::default_initializable<Resource>
    = default;

    /** @brief Construct the directory from `args`, typically a resource; the pages use the same one. */
    template <typename... Args>
        requires(sizeof...(Args) > 0) && (!std::same_as<std::remove_cvref_t<Args>, paged_sparse_index> && ...) &&
                std::constructible_from<directory_storage, Args...>
    explicit paged_sparse_index(Args&&... args) : directory_{std::forward<Args>(args)...} {}

    paged_sparse_index(const paged_sparse_index&) = delete;
    paged_sparse_index& operator=(const paged_sparse_index&) = delete;

    paged_sparse_index(paged_sparse_index&& other) noexcept : directory_{std::move(other.directory_)}, pages_{std::exchange(other.pages_, 0)} {}

    paged_sparse_index& operator=(paged_sparse_index&& other) noexcept {
        if (this != &other) {
            release();
            directory_ = std::move(other.directory_);
            pages_ = std::exchange(other.pages_, 0);
        }
        return *this;
    }

    ~paged_sparse_index() { release(); }

    constexpr size_type covered() const noexcept { return pages_ * PageSize; }

    size_type get(const size_type at) const noexcept {
        const size_type page{at >> page_shift};
        if (page >= pages_) {
            return npos;
        }
        const size_type* slots{directory_.data()[page]};
        return slots ? slots[at & page_mask] : npos;
    }

    void set(const size_type at, const size_type index) noexcept {
        const size_type page{at >> page_shift};
        assert(page < pages_ && directory_.data()[page] != nullptr && "paged_sparse_index: subscript not reserved");
        directory_.data()[page][at & page_mask] = index;
    }

    /**
     * @brief Make sure the page holding `at` exists.
     *
     * Two allocations at most, both of which leave the index unchanged on failure:
     * the directory grows through the block protocol, and the page itself is only
     * published into the directory once it is allocated and tombstoned.
     *
     * @return `false` when the resource could not supply the space.
     */
    bool reserve_for(const size_type at) {
        assert(at != npos && "paged_sparse_index: npos is the reserved tombstone, not a subscript");

        const size_type page{at >> page_shift};

        if (page >= pages_) {
            if (page >= directory_.capacity() && !relocate_grow(directory_, page + 1, pages_)) {
                return false;
            }
            /* Every directory slot is a live pointer, so a lookup can read one
             * without tracking a separate initialised count. */
            for (size_type i{pages_}; i < directory_.capacity(); ++i) {
                std::construct_at(directory_.data() + i, nullptr);
            }
            pages_ = directory_.capacity();
        }

        size_type*& slot{directory_.data()[page]};
        if (!slot) {
            size_type* fresh{detail::allocate_slots<size_type, alignof(size_type)>(directory_.resource(), PageSize)};
            if (!fresh) {
                return false;
            }
            std::uninitialized_fill_n(fresh, PageSize, npos);
            slot = fresh;
        }

        return true;
    }

    /** @brief Directory entries, i.e. `covered() / PageSize`. */
    constexpr size_type page_count() const noexcept { return pages_; }

    /**
     * @brief Pages actually allocated, which is what the index costs beyond its directory.
     *
     * O(page_count()); an observer for tests and diagnostics, not a hot path.
     */
    size_type live_page_count() const noexcept {
        const size_type* const* pages{directory_.data()};
        size_type live{};
        for (size_type i{}; i < pages_; ++i) {
            if (pages[i]) {
                ++live;
            }
        }
        return live;
    }

private:
    static constexpr size_type page_shift{static_cast<size_type>(std::countr_zero(PageSize))};
    static constexpr size_type page_mask{PageSize - 1};

    directory_storage directory_{};
    size_type pages_{};

    void release() noexcept {
        size_type** pages{directory_.data()};

        for (size_type i{}; i < pages_; ++i) {
            if (pages[i]) {
                std::destroy_n(pages[i], PageSize);
                detail::free_slots<size_type, alignof(size_type)>(directory_.resource(), pages[i], PageSize);
            }
        }

        std::destroy_n(pages, pages_);
        pages_ = 0;
    }
};

/* ============================================================================
 * Concept verification
 * ============================================================================ */

static_assert(sparse_index<flat_sparse_index<dynamic_storage<std::size_t>>>);
static_assert(flat_sparse_index<dynamic_storage<std::size_t>>::growable);
static_assert(flat_sparse_index<dynamic_storage<std::size_t>>::relocatable);

/* An inline-backed flat index neither grows nor moves, which is what makes
 * sparse_set over inline_storage a fixed, non-movable set. */
static_assert(sparse_index<flat_sparse_index<inline_storage<std::size_t, 32>>>);
static_assert(!flat_sparse_index<inline_storage<std::size_t, 32>>::growable);
static_assert(!flat_sparse_index<inline_storage<std::size_t, 32>>::relocatable);

static_assert(sparse_index<paged_sparse_index<>>);
static_assert(paged_sparse_index<>::page_size == default_sparse_page_size);
static_assert(sizeof(std::size_t) * default_sparse_page_size == 4096);

} // namespace libmem

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
