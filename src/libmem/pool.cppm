/**
 * @file pool.cppm
 * @brief Pointer-stable, unordered typed container built on top of `multislab`.
 *
 * `pool<T>` is a node-based, bucketed sequence container that:
 *
 *   - Allocates fixed-size blocks (one per `T`) out of an auto-expanding
 *     `libmem::multislab`, deriving the block size from `sizeof(T)` /
 *     `alignof(T)`.
 *   - Guarantees **pointer and iterator stability** for every live element
 *     until that specific element is erased: insertions never relocate
 *     previously inserted elements.
 *   - Reuses freed slots before growing (first-free-bit allocation), so
 *     insertion is amortised O(1). Insertion order is **not preserved**.
 *   - Provides a forward range over the live elements suitable for
 *     `std::ranges` pipelines (`operator|`, views, algorithms, …).
 *
 * @code
 *     libmem::pool<some_struct> p{};
 *     auto it = p.emplace(42, 3.14);
 *     for (auto& s : p) { ... }
 *     auto squares = p | std::views::transform([](auto& s){ return s.x * s.x; });
 *     p.erase(it);
 * @endcode
 */
module;

#include <cassert>

export module libmem:pool;

import :concepts;
import :multislab;
import std;

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-owning-memory)

namespace libmem {

namespace detail {

/** @brief Round `n` up to the nearest non-zero multiple of `m`. */
consteval std::size_t round_up_to_multiple(const std::size_t n, const std::size_t m) noexcept {
    const std::size_t v{n == 0 ? m : n};
    return ((v + m - 1) / m) * m;
}

/**
 * @brief Block size used to store one `T` inside the underlying `multislab`.
 *
 * Rounded up to the cache-line constraint enforced by `valid_block_size`,
 * and never smaller than the alignment requirement of `T`.
 */
template <typename T> inline constexpr std::size_t pool_block_size_v{round_up_to_multiple(std::max(sizeof(T), alignof(T)), cache_line_size)};

/**
 * @brief Default number of blocks per slab page, targeting a ~16 KiB page
 *        and never going below a small floor to keep growth amortised.
 */
template <typename T> consteval std::uint32_t default_pool_blocks_per_slab() noexcept {
    constexpr std::size_t target_page_bytes{1 << 14};
    constexpr std::size_t per_block{pool_block_size_v<T>};
    constexpr std::size_t derived{target_page_bytes / per_block};
    constexpr std::size_t floor{8};
    return static_cast<std::uint32_t>(derived < floor ? floor : derived);
}

} // namespace detail

/* ============================================================================
 * pool — auto-expanding, pointer-stable typed container
 * ============================================================================ */

/**
 * @brief Pointer-stable unordered typed container backed by a `libmem::multislab`.
 *
 * @tparam T              Element type — must be an object type.
 * @tparam BlocksPerSlab  Number of `T` slots per slab page (auto-derived).
 * @tparam Resource       Backing memory resource (see `memory_resource`).
 * @tparam Policy         Empty-slab hysteresis policy (see `shrink_policy`).
 */
export template <typename T, std::uint32_t BlocksPerSlab = detail::default_pool_blocks_per_slab<T>(), memory_resource Resource = default_resource,
    shrink_policy Policy = threshold_policy>
    requires std::is_object_v<T> && (BlocksPerSlab > 0)
class pool {
    using pool_type = multislab<detail::pool_block_size_v<T>, BlocksPerSlab, Resource, Policy>;
    using pool_iterator = typename pool_type::iterator;

public:
    /* ========================================================================
     * Member types
     * ======================================================================== */

    using value_type = T;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    /** @brief Block size (in bytes) of the underlying `multislab`. */
    static constexpr std::size_t block_size{pool_type::block_size};

    /** @brief Number of `T` slots per slab page. */
    static constexpr std::uint32_t blocks_per_slab{pool_type::blocks_per_slab};

    /* ========================================================================
     * Iterator — typed wrapper around the underlying multislab iterator
     * ======================================================================== */

    /**
     * @brief Forward iterator over the live elements.
     *
     * @tparam Const When `true`, dereferencing yields `const T&`.
     */
    template <bool Const> class basic_iterator {
        friend class pool;
        template <bool> friend class basic_iterator;

    public:
        using value_type = T;
        using reference = std::conditional_t<Const, const T&, T&>;
        using pointer = std::conditional_t<Const, const T*, T*>;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept = std::forward_iterator_tag;

        constexpr basic_iterator() noexcept = default;

        /**
         * @brief Heterogeneous conversion (mutable → const).
         *
         * Templated on a distinct `OtherConst` so this does not displace the
         * implicit copy constructor that the language synthesises for
         * `basic_iterator<Const>`.
         */
        template <bool OtherConst>
            requires(Const && !OtherConst)
        constexpr basic_iterator(const basic_iterator<OtherConst>& other) noexcept : inner_{other.inner_} {}

        constexpr reference operator*(this auto&& self) noexcept { return *static_cast<pointer>(*self.inner_); }
        constexpr pointer operator->(this auto&& self) noexcept { return static_cast<pointer>(*self.inner_); }

        constexpr basic_iterator& operator++() noexcept {
            ++inner_;
            return *this;
        }
        constexpr basic_iterator operator++(int) noexcept {
            auto tmp{*this};
            ++inner_;
            return tmp;
        }

        /** @brief Iterator-to-iterator equality (defaulted on the wrapped iterator). */
        friend constexpr bool operator==(const basic_iterator& lhs, const basic_iterator& rhs) noexcept { return lhs.inner_ == rhs.inner_; }

        /** @brief Heterogeneous (mutable / const) equality. */
        template <bool OtherConst>
            requires(Const != OtherConst)
        constexpr bool operator==(const basic_iterator<OtherConst>& rhs) const noexcept {
            return inner_ == rhs.inner_;
        }

        constexpr bool operator==(std::default_sentinel_t) const noexcept { return inner_ == std::default_sentinel; }

    private:
        constexpr explicit basic_iterator(pool_iterator inner) noexcept : inner_{inner} {}

        pool_iterator inner_{};
    };

    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

    /* ========================================================================
     * Construction / destruction
     * ======================================================================== */

    constexpr pool() noexcept = default;

    /** @brief Construct with a custom resource. */
    constexpr explicit pool(Resource resource) noexcept : pool_{std::move(resource)} {}

    /** @brief Construct with a custom resource and policy. */
    constexpr pool(Resource resource, Policy policy) noexcept : pool_{std::move(resource), std::move(policy)} {}

    /** @brief Construct with a custom shrink policy. */
    constexpr explicit pool(Policy policy) noexcept : pool_{std::move(policy)} {}

    /** @brief Construct with a hard cap on the number of slab pages (0 = unlimited). */
    constexpr explicit pool(const std::uint32_t max_slabs) noexcept : pool_{max_slabs} {}

    /** @brief Construct from an arbitrary input range of values. */
    template <std::ranges::input_range R>
        requires std::constructible_from<T, std::ranges::range_reference_t<R>> && (!std::same_as<std::remove_cvref_t<R>, pool>)
    explicit pool(R&& r) {
        insert_range(std::forward<R>(r));
    }

    /** @brief Construct from an initializer list. */
    pool(std::initializer_list<T> il)
        requires std::copy_constructible<T>
    {
        for (const auto& v : il) {
            emplace(v);
        }
    }

    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;

    constexpr pool(pool&& other) noexcept : pool_{std::move(other.pool_)}, size_{std::exchange(other.size_, 0)} {}

    constexpr pool& operator=(pool&& other) noexcept {
        if (this != &other) {
            clear();
            pool_ = std::move(other.pool_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~pool() { clear(); }

    /* ========================================================================
     * Capacity
     * ======================================================================== */

    /** @brief True when there are no live elements. */
    constexpr bool empty() const noexcept { return size_ == 0; }

    /** @brief Number of live (constructed) elements. */
    constexpr size_type size() const noexcept { return size_; }

    /** @brief Theoretical upper bound on `size()`. */
    constexpr size_type max_size() const noexcept { return std::numeric_limits<size_type>::max() / sizeof(T); }

    /** @brief Number of slab pages currently held by the underlying pool. */
    constexpr std::uint32_t slab_count() const noexcept { return pool_.slab_count(); }

    /** @brief Number of currently empty (retained) slab pages. */
    constexpr std::uint32_t empty_slab_count() const noexcept { return pool_.empty_slab_count(); }

    /* ========================================================================
     * Iteration — deducing-this share between const / non-const overloads
     * ======================================================================== */

    /** @brief Iterator to the first live element. */
    constexpr auto begin(this auto&& self) noexcept {
        using self_t = std::remove_reference_t<decltype(self)>;
        if constexpr (std::is_const_v<self_t>) {
            return const_iterator{self.pool_.begin()};
        } else {
            return iterator{self.pool_.begin()};
        }
    }

    /** @brief Sentinel marking the end of iteration. */
    static constexpr std::default_sentinel_t end() noexcept { return {}; }

    /** @brief `const` iterator to the first live element. */
    constexpr const_iterator cbegin() const noexcept { return const_iterator{pool_.begin()}; }

    /** @brief `const` sentinel. */
    static constexpr std::default_sentinel_t cend() noexcept { return {}; }

    /* ========================================================================
     * Modifiers
     * ======================================================================== */

    /**
     * @brief Construct a new element in place.
     * @return Iterator to the newly constructed element.
     *
     * Existing iterators and pointers to other elements remain valid.
     */
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    iterator emplace(Args&&... args) {
        void* mem{pool_.allocate()};
        assert(mem != nullptr && "pool: backing resource exhausted");

        auto* p{::new (mem) T(std::forward<Args>(args)...)};
        ++size_;
        return iterator{pool_.make_iterator(p)};
    }

    /** @brief Copy-insert `value`. */
    iterator insert(const T& value)
        requires std::copy_constructible<T>
    {
        return emplace(value);
    }

    /** @brief Move-insert `value`. */
    iterator insert(T&& value)
        requires std::move_constructible<T>
    {
        return emplace(std::move(value));
    }

    /**
     * @brief Insert every element of `r`, one at a time.
     * @return Number of elements inserted.
     */
    template <std::ranges::input_range R>
        requires std::constructible_from<T, std::ranges::range_reference_t<R>>
    size_type insert_range(R&& r) {
        size_type n{};
        for (auto&& e : r) {
            emplace(std::forward<decltype(e)>(e));
            ++n;
        }
        return n;
    }

    /** @brief Insert every element of an initializer list. */
    size_type insert(std::initializer_list<T> il)
        requires std::copy_constructible<T>
    {
        return insert_range(il);
    }

    /**
     * @brief Destroy the element referenced by `it` and free its slot.
     * @return Iterator pointing to the element following the erased one,
     *         or `end()` if `it` referenced the last live element.
     *
     * Iterators and pointers to other elements remain valid.
     */
    iterator erase(const_iterator it) noexcept(std::is_nothrow_destructible_v<T>) {
        assert(size_ > 0);

        pool_iterator inner{it.inner_};
        pool_iterator next{inner};
        ++next;

        auto* raw{*inner};
        static_cast<T*>(raw)->~T();
        pool_.deallocate(raw);
        --size_;

        return iterator{next};
    }

    /** @brief Destroy all live elements and release all slab pages. */
    constexpr void clear() noexcept(std::is_nothrow_destructible_v<T>) {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (void* raw : pool_) {
                static_cast<T*>(raw)->~T();
            }
        }
        pool_.destroy();
        size_ = 0;
    }

    /* ========================================================================
     * Observers — direct, deducing-this access to underlying pool knobs
     * ======================================================================== */

    /** @brief Access the backing memory resource. */
    constexpr auto& resource(this auto&& self) noexcept { return self.pool_.resource(); }

    /** @brief Access the empty-slab shrink policy. */
    constexpr auto& policy(this auto&& self) noexcept { return self.pool_.policy(); }

private:
    pool_type pool_{};
    size_type size_{};
};

/* Concept verification: pool should be a forward range with proper iterators. */
static_assert(std::ranges::forward_range<pool<std::int32_t>>);
static_assert(std::forward_iterator<pool<std::int32_t>::iterator>);
static_assert(std::forward_iterator<pool<std::int32_t>::const_iterator>);

} // namespace libmem

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-owning-memory)
