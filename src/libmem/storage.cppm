/**
 * @file storage.cppm
 * @brief Raw element storage: the layer every libmem container builds its buffer on.
 *
 * A `storage` owns *space* for `capacity()` objects of `value_type` and nothing
 * else. It never constructs, destroys, or counts elements; the container that
 * holds it does all of that.
 *
 * Four implementations, differing on where the slots live and whether the
 * extent is fixed at compile time:
 *
 *   - `inline_storage<T, N>`:     N slots inside the object. No allocation.
 *   - `fixed_storage<T, N, R>`:   N slots from a `memory_resource`. Static extent,
 *                                 so `sizeof` stays small no matter how big N is.
 *   - `dynamic_storage<T, R>`:    runtime capacity, geometric growth (`std::vector`-like).
 *   - `small_storage<T, N, R>`:   N slots inside the object, spilling to a
 *                                 `memory_resource` once outgrown (SBO).
 *
 * @code
 *     libmem::spsc_ring<command, 64> small{};                    // inline slots
 *     libmem::heap_spsc_ring<command, 1 << 20> big{};            // same ring, slots on the heap
 *
 *     libmem::arena scratch{1 << 20};
 *     libmem::sparse_set<entity, libmem::dynamic_storage<entity, libmem::resource_ref<libmem::arena>>>
 *         ids{libmem::resource_ref{scratch}};                    // grows out of the arena
 * @endcode
 *
 * @section growth The growth protocol
 *
 * Growth is three steps, not one:
 *
 *   1. `reserve_block(n)` allocates a fresh block. The current block is untouched.
 *   2. The container relocates its live elements into the new block.
 *   3. `adopt(block)` releases the old block and takes the new one, or
 *      `discard(block)` throws the new one away and keeps the old.
 *
 * A failure at any step leaves the container exactly as it was, including a move
 * constructor throwing partway through step 2. `relocate_grow` implements the
 * single-array case.
 */
module;

#include <cassert>

export module libmem:storage;

import :concepts;
import std;

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-owning-memory, cppcoreguidelines-pro-bounds-pointer-arithmetic)

namespace libmem {

/* ============================================================================
 * Extent marker
 * ============================================================================ */

/**
 * @brief `static_capacity` value marking a storage whose extent is a runtime property.
 *
 * Spelled as `std::dynamic_extent` so it reads the same as `std::span`.
 */
export inline constexpr std::size_t dynamic_extent{std::dynamic_extent};

/* ============================================================================
 * Blocks
 * ============================================================================ */

/**
 * @brief An allocated, still-uninitialised run of slots handed out by `reserve_block`.
 *
 * `capacity` is the number of slots actually allocated, which may exceed what was
 * asked for: geometric growth happens inside `reserve_block`, and the sized
 * `deallocate` on the way back out has to be given the real figure. A container
 * adopting a block therefore adopts this `capacity`, not the one it requested.
 *
 * A default-constructed (or failed) block is falsy.
 */
export template <typename T> struct storage_block {
    T* data{};
    std::size_t capacity{};

    constexpr explicit operator bool() const noexcept { return data != nullptr; }
};

/* ============================================================================
 * Concepts
 * ============================================================================ */

/**
 * @brief Raw space for `capacity()` objects of `value_type`.
 *
 * Construction requirements are deliberately absent: a storage over a stateful
 * resource is not default-constructible, and an inline storage is not movable, so
 * each container states for itself what it needs of its storage.
 *
 * `rebind<U>` yields the same storage kind holding `U` instead, keeping any
 * injected resource and resetting the alignment to `alignof(U)`, so a container
 * can hold several parallel arrays of different element types over one storage
 * template argument.
 */
export template <typename S>
concept storage = requires(S& s, const S& cs) {
    typename S::value_type;
    typename S::size_type;
    typename S::template rebind<int>;

    /* dynamic_extent when the extent is a runtime property. */
    { S::static_capacity } -> std::convertible_to<std::size_t>;
    { S::alignment } -> std::convertible_to<std::size_t>;
    /* Whether moving the storage object transfers the slots (heap-backed) or
     * cannot (inline). Containers use it to decide whether they are movable. */
    { S::relocatable } -> std::convertible_to<bool>;

    { s.data() } -> std::same_as<typename S::value_type*>;
    { cs.data() } -> std::same_as<const typename S::value_type*>;
    { cs.capacity() } -> std::same_as<typename S::size_type>;
};

/** @brief A `storage` whose `value_type` is exactly `T`. */
export template <typename S, typename T>
concept storage_for = storage<S> && std::same_as<typename S::value_type, T>;

/** @brief A `storage` that can hand out a larger block; see @ref growth. */
export template <typename S>
concept growable_storage = storage<S> && requires(S& s, typename S::size_type n, storage_block<typename S::value_type> block) {
    { s.reserve_block(n) } -> std::same_as<storage_block<typename S::value_type>>;
    { s.adopt(block) } -> std::same_as<void>;
    { s.discard(block) } -> std::same_as<void>;
};

/** @brief A `storage` whose extent is fixed at compile time. */
export template <typename S>
concept fixed_extent_storage = storage<S> && (S::static_capacity != dynamic_extent);

/**
 * @brief A `storage` that exposes the resource its slots came from, and can be rebuilt against it.
 *
 * What lets a container clone itself into the same resource rather than a
 * default-constructed one.
 *
 * @warning A storage that should model this but does not fails quietly: the clone
 *          gets a default-constructed resource, and a `resource_ref` one is then
 *          null, so the first allocation asserts at some later call site rather
 *          than at the clone.
 */
export template <typename S>
concept resourced_storage = storage<S> && requires(S& s) {
    { s.resource() };
    requires std::constructible_from<S, decltype(s.resource())>;
};

/**
 * @brief A `growable_storage` that can hand its off-object block over to another instance of itself.
 *
 * The run-time counterpart to `relocatable`, for a storage whose slots are only
 * sometimes transferable. `adopt_from` succeeds when the source holds an
 * off-object block, and reports `false` when the caller must move the elements
 * itself.
 *
 * @note Distinct from `adopt`, which takes a block the caller reserved. This takes
 *       over a live storage's block *and its resource*, so a block is never
 *       released through a resource that did not supply it.
 */
export template <typename S>
concept transferable_storage = growable_storage<S> && requires(S& s, S& other) {
    { s.adopt_from(other) } -> std::same_as<bool>;
};

/* ============================================================================
 * Shared allocation helpers
 * ============================================================================ */

namespace detail {

/** @brief A valid alignment for slots of `T`: a power of two, and at least what `T` needs. */
template <typename T, std::size_t Align>
concept valid_storage_alignment = valid_alignment<Align> && (Align >= alignof(T));

/**
 * @brief Take `count` slots for `T` from `resource`.
 *
 * The aligned and unaligned resource overloads are selected on one condition
 * here and on the same condition in `free_slots`; releasing an aligned
 * allocation through the unaligned `deallocate` is undefined, so the two must
 * never diverge.
 */
template <typename T, std::size_t Align, memory_resource Resource> T* allocate_slots(Resource& resource, const std::size_t count) {
    if constexpr (aligned_memory_resource<Resource>) {
        return static_cast<T*>(resource.allocate(count * sizeof(T), Align));
    } else {
        static_assert(Align <= default_alignment, "storage: over-aligned slots need an aligned_memory_resource. Give the storage a resource with "
                                                  "allocate(size, align) / deallocate(ptr, size, align), or drop its alignment to default_alignment.");
        return static_cast<T*>(resource.allocate(count * sizeof(T)));
    }
}

/** @brief Release slots taken by `allocate_slots`; branches identically to it. */
template <typename T, std::size_t Align, memory_resource Resource> void free_slots(Resource& resource, T* ptr, const std::size_t count) noexcept {
    if (!ptr) {
        return;
    }
    if constexpr (aligned_memory_resource<Resource>) {
        resource.deallocate(ptr, count * sizeof(T), Align);
    } else {
        resource.deallocate(ptr, count * sizeof(T));
    }
}

/* ----------------------------------------------------------------------------
 * Constexpr-usable stand-ins for the specialized memory algorithms
 *
 * libc++ has not yet made `std::uninitialized_move_n` and friends `constexpr`
 * (P2283); libstdc++ has. Calling them directly would make every inline container
 * constant-evaluable on GCC and not on Clang. These do the same work through
 * `std::construct_at`, which has been constexpr since C++20, and keep the same
 * roll-back-on-throw guarantee.
 * ---------------------------------------------------------------------------- */

/** @brief Move-construct `n` objects from `first` into the raw slots at `out`. */
template <typename T> constexpr void relocate_n(T* first, const std::size_t n, T* out) {
    if constexpr (std::is_nothrow_move_constructible_v<T>) {
        for (std::size_t i{}; i < n; ++i) {
            std::construct_at(out + i, std::move(first[i]));
        }
    } else {
        std::size_t built{};
        try {
            for (; built < n; ++built) {
                std::construct_at(out + built, std::move(first[built]));
            }
        } catch (...) {
            std::destroy_n(out, built);
            throw;
        }
    }
}

/** @brief Value-initialise `n` raw slots at `first`. */
template <typename T> constexpr void value_construct_n(T* first, const std::size_t n) {
    if constexpr (std::is_nothrow_default_constructible_v<T>) {
        for (std::size_t i{}; i < n; ++i) {
            std::construct_at(first + i);
        }
    } else {
        std::size_t built{};
        try {
            for (; built < n; ++built) {
                std::construct_at(first + built);
            }
        } catch (...) {
            std::destroy_n(first, built);
            throw;
        }
    }
}

/** @brief Copy-construct `value` into `n` raw slots at `first`. */
template <typename T, typename U> constexpr void fill_construct_n(T* first, const std::size_t n, const U& value) {
    if constexpr (std::is_nothrow_constructible_v<T, const U&>) {
        for (std::size_t i{}; i < n; ++i) {
            std::construct_at(first + i, value);
        }
    } else {
        std::size_t built{};
        try {
            for (; built < n; ++built) {
                std::construct_at(first + built, value);
            }
        } catch (...) {
            std::destroy_n(first, built);
            throw;
        }
    }
}

/** @brief Smallest capacity a growing storage ever allocates, so early inserts do not re-grow every time. */
template <typename T> inline constexpr std::size_t growth_floor{sizeof(T) > 256 ? 1 : 256 / sizeof(T)};

/**
 * @brief Capacity to allocate when `current` slots must become at least `requested`.
 *
 * `max(requested, 2 * current, growth_floor)`, so repeated single-element growth
 * stays amortised O(1). Shared by every growing storage.
 */
template <typename T> constexpr std::size_t next_capacity(const std::size_t current, const std::size_t requested) noexcept {
    const std::size_t doubled{current > (std::numeric_limits<std::size_t>::max() / 2) ? current : current * 2};
    return std::max({requested, doubled, growth_floor<T>});
}

} // namespace detail

/* ============================================================================
 * inline_storage: slots inside the object
 * ============================================================================ */

namespace detail {

/**
 * @brief Whether `T`'s inline slots can be a real `T[N]` rather than raw bytes.
 *
 * Trivial default construction lets the array be left alone at runtime, and
 * trivial destruction makes `construct_at` over a slot that already holds an
 * object well-defined. Those two, not `is_trivially_copyable`, are what the
 * constexpr path needs.
 */
template <typename T>
concept constexpr_inline_slots = std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>;

/**
 * @brief The slot array behind `inline_storage`, chosen so `T[N]` is used where it can be.
 *
 * Raw bytes cannot be reinterpreted during constant evaluation, so a byte array
 * shuts `inline_vector` out of constexpr entirely. A real `T[N]` has no such
 * problem and costs nothing extra: P1331 permits leaving it trivially
 * default-initialised inside a `constexpr` constructor, so it is not zeroed at
 * runtime either.
 */
template <typename T, std::size_t N, std::size_t Align, bool Trivial = constexpr_inline_slots<T>> struct inline_slots {
    /* Deliberately uninitialised. A braced initialiser here would zero the whole
     * array on every construction, which is exactly what this storage exists to
     * avoid. */
    alignas(Align) T slots[N];

    constexpr T* data() noexcept { return slots; }
    constexpr const T* data() const noexcept { return slots; }
};

/** Non-trivial `T`: raw bytes, and therefore no constant evaluation. */
template <typename T, std::size_t N, std::size_t Align> struct inline_slots<T, N, Align, false> {
    alignas(Align) std::byte slots[N * sizeof(T)];

    T* data() noexcept { return reinterpret_cast<T*>(slots); }
    const T* data() const noexcept { return reinterpret_cast<const T*>(slots); }
};

} // namespace detail

/**
 * @brief `N` uninitialised slots for `T` held inside the storage object itself.
 *
 * Allocates nothing, so a container built on it needs no resource and cannot
 * fail to obtain space. Neither copyable nor movable: the bytes may hold live
 * objects whose relocation only the owning container can perform.
 *
 * @tparam T     Element type.
 * @tparam N     Slot count.
 * @tparam Align Alignment of the slot array; at least `alignof(T)`. Raising it
 *               is how a container puts its buffer on a cache line.
 *
 * @note Usable in constant expressions when `T` is trivially default-constructible
 *       and trivially destructible, which is the same bar `std::inplace_vector`
 *       sets. A `T` outside it falls back to a byte array, and a container over it
 *       still works, just not at compile time.
 */
export template <typename T, std::size_t N, std::size_t Align = alignof(T)>
    requires std::is_object_v<T> && (N > 0) && detail::valid_storage_alignment<T, Align>
class inline_storage {
public:
    using value_type = T;
    using size_type = std::size_t;
    template <typename U> using rebind = inline_storage<U, N>;

    static constexpr size_type static_capacity{N};
    static constexpr size_type alignment{Align};
    static constexpr bool relocatable{false};

    /** @brief Whether this storage can be used during constant evaluation. */
    static constexpr bool constexpr_usable{detail::constexpr_inline_slots<T>};

    constexpr inline_storage() noexcept = default;

    inline_storage(const inline_storage&) = delete;
    inline_storage& operator=(const inline_storage&) = delete;
    inline_storage(inline_storage&&) = delete;
    inline_storage& operator=(inline_storage&&) = delete;

    constexpr T* data() noexcept { return slots_.data(); }
    constexpr const T* data() const noexcept { return slots_.data(); }

    static constexpr size_type capacity() noexcept { return N; }

private:
    /* No initialiser: braces here would value-initialise the array and zero it on
     * every construction. */
    detail::inline_slots<T, N, Align> slots_;
};

/* ============================================================================
 * fixed_storage: compile-time extent, slots on a resource
 * ============================================================================ */

/**
 * @brief `N` uninitialised slots for `T` taken from a `memory_resource`.
 *
 * The extent is still a compile-time constant, so a container keeps whatever it
 * derived from it (a power-of-two mask, a compile-time bound check) while
 * `sizeof(storage)` stays a pointer plus the resource.
 *
 * @note The single allocation happens at construction, and exhaustion is
 *       asserted rather than reported: a fixed-extent buffer that could not be
 *       obtained has no valid degraded state.
 */
export template <typename T, std::size_t N, memory_resource Resource = default_resource, std::size_t Align = alignof(T)>
    requires std::is_object_v<T> && (N > 0) && detail::valid_storage_alignment<T, Align>
class fixed_storage {
public:
    using value_type = T;
    using size_type = std::size_t;
    using resource_type = Resource;
    template <typename U> using rebind = fixed_storage<U, N, Resource>;

    static constexpr size_type static_capacity{N};
    static constexpr size_type alignment{Align};
    static constexpr bool relocatable{true};

    fixed_storage()
        requires std::default_initializable<Resource>
        : slots_{detail::allocate_slots<T, Align>(resource_, N)} {
        assert(slots_ != nullptr && "fixed_storage: resource could not supply the slots");
    }

    explicit fixed_storage(Resource resource) : resource_{std::move(resource)}, slots_{detail::allocate_slots<T, Align>(resource_, N)} {
        assert(slots_ != nullptr && "fixed_storage: resource could not supply the slots");
    }

    fixed_storage(const fixed_storage&) = delete;
    fixed_storage& operator=(const fixed_storage&) = delete;

    fixed_storage(fixed_storage&& other) noexcept : resource_{std::move(other.resource_)}, slots_{std::exchange(other.slots_, nullptr)} {}

    fixed_storage& operator=(fixed_storage&& other) noexcept {
        if (this != &other) {
            release();
            resource_ = std::move(other.resource_);
            slots_ = std::exchange(other.slots_, nullptr);
        }
        return *this;
    }

    ~fixed_storage() { release(); }

    T* data() noexcept { return slots_; }
    const T* data() const noexcept { return slots_; }

    /**
     * @brief `N`, or 0 once moved from.
     *
     * @warning Not `static constexpr`. A moved-from storage has handed its slots
     *          away, and reporting `N` while `data()` is null would let a container
     *          walk slots that do not exist. `static_capacity` is the compile-time
     *          constant.
     */
    constexpr size_type capacity() const noexcept { return slots_ ? N : 0; }

    /** @brief Access the backing resource. */
    constexpr auto& resource(this auto&& self) noexcept { return self.resource_; }

private:
    Resource resource_{};
    T* slots_{};

    void release() noexcept {
        detail::free_slots<T, Align>(resource_, slots_, N);
        slots_ = nullptr;
    }
};

/* ============================================================================
 * dynamic_storage: runtime extent, geometric growth
 * ============================================================================ */

/**
 * @brief Uninitialised slots for `T` from a `memory_resource`, growable at runtime.
 *
 * Starts empty (no allocation) and grows geometrically through the block
 * protocol described in @ref growth. This is the `std::vector`-shaped storage:
 * capacity is a runtime property, and growth moves the elements, so a container
 * over it invalidates pointers and iterators when it grows.
 */
export template <typename T, memory_resource Resource = default_resource, std::size_t Align = alignof(T)>
    requires std::is_object_v<T> && detail::valid_storage_alignment<T, Align>
class dynamic_storage {
public:
    using value_type = T;
    using size_type = std::size_t;
    using resource_type = Resource;
    template <typename U> using rebind = dynamic_storage<U, Resource>;

    static constexpr size_type static_capacity{dynamic_extent};
    static constexpr size_type alignment{Align};
    static constexpr bool relocatable{true};

    /** @brief Smallest capacity ever allocated, so early inserts do not re-grow every time. */
    static constexpr size_type min_capacity{detail::growth_floor<T>};

    constexpr dynamic_storage()
        requires std::default_initializable<Resource>
    = default;

    constexpr explicit dynamic_storage(Resource resource) noexcept : resource_{std::move(resource)} {}

    dynamic_storage(const dynamic_storage&) = delete;
    dynamic_storage& operator=(const dynamic_storage&) = delete;

    constexpr dynamic_storage(dynamic_storage&& other) noexcept
        : resource_{std::move(other.resource_)}, slots_{std::exchange(other.slots_, nullptr)}, capacity_{std::exchange(other.capacity_, 0)} {}

    constexpr dynamic_storage& operator=(dynamic_storage&& other) noexcept {
        if (this != &other) {
            release();
            resource_ = std::move(other.resource_);
            slots_ = std::exchange(other.slots_, nullptr);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }

    ~dynamic_storage() { release(); }

    constexpr T* data() noexcept { return slots_; }
    constexpr const T* data() const noexcept { return slots_; }

    constexpr size_type capacity() const noexcept { return capacity_; }

    /**
     * @brief Allocate a block of at least `n` slots, leaving the current one live.
     *
     * The returned capacity is at least `max(n, 2 * capacity(), min_capacity)`, so
     * repeated single-element growth stays amortised O(1).
     *
     * @return A falsy block when the resource could not supply the memory.
     */
    [[nodiscard]] storage_block<T> reserve_block(const size_type n) {
        const size_type target{detail::next_capacity<T>(capacity_, n)};
        T* ptr{detail::allocate_slots<T, Align>(resource_, target)};
        if (!ptr) {
            return {};
        }
        return {ptr, target};
    }

    /**
     * @brief Release the current block and take `block` as the new one.
     * @pre The container has already relocated its live elements into `block`,
     *      and no live object remains in the current block.
     */
    void adopt(const storage_block<T> block) noexcept {
        assert(block.data != nullptr && "dynamic_storage: cannot adopt an empty block");
        detail::free_slots<T, Align>(resource_, slots_, capacity_);
        slots_ = block.data;
        capacity_ = block.capacity;
    }

    /**
     * @brief Give `block` back without adopting it; the current block stays live.
     * @pre `block` holds no live objects.
     */
    void discard(const storage_block<T> block) noexcept { detail::free_slots<T, Align>(resource_, block.data, block.capacity); }

    /** @brief Access the backing resource. */
    constexpr auto& resource(this auto&& self) noexcept { return self.resource_; }

private:
    Resource resource_{};
    T* slots_{};
    size_type capacity_{};

    void release() noexcept {
        detail::free_slots<T, Align>(resource_, slots_, capacity_);
        slots_ = nullptr;
        capacity_ = 0;
    }
};

/* ============================================================================
 * small_storage: inline slots that spill to a resource
 * ============================================================================ */

/**
 * @brief `N` slots inside the object, spilling to a `memory_resource` once outgrown.
 *
 * `capacity()` starts at `N` with `data()` pointing into the object. The first
 * growth past `N` takes a block through the protocol in @ref growth.
 *
 * @tparam T        Element type.
 * @tparam N        Inline slot count.
 * @tparam Resource Resource the spilled block comes from.
 * @tparam Align    Alignment of both the inline array and the spilled block.
 *
 * @note Once spilled, always spilled: nothing moves back into the inline slots
 *       when the element count drops.
 *
 * @note `relocatable` is `false`; moving the storage object cannot transfer slots
 *       that are the object. `adopt_from` is the run-time path a container uses
 *       instead.
 *
 * @warning `rebind<U>` keeps `N` as a *slot count*, not a byte budget. A
 *          `sparse_set<entity, small_storage<entity, 64>>` gets 64 inline dense
 *          slots **and** 64 inline `size_type` sparse slots.
 */
export template <typename T, std::size_t N, memory_resource Resource = default_resource, std::size_t Align = alignof(T)>
    requires std::is_object_v<T> && (N > 0) && detail::valid_storage_alignment<T, Align>
class small_storage {
public:
    using value_type = T;
    using size_type = std::size_t;
    using resource_type = Resource;
    template <typename U> using rebind = small_storage<U, N, Resource>;

    /* Dynamic: the capacity is N until it is not, so nothing may bake N in. */
    static constexpr size_type static_capacity{dynamic_extent};
    static constexpr size_type alignment{Align};
    static constexpr bool relocatable{false};

    /** @brief Slots available before the first spill. */
    static constexpr size_type inline_capacity{N};

    small_storage() noexcept
        requires std::default_initializable<Resource>
        : slots_{inline_data()} {}

    explicit small_storage(Resource resource) noexcept : resource_{std::move(resource)}, slots_{inline_data()} {}

    small_storage(const small_storage&) = delete;
    small_storage& operator=(const small_storage&) = delete;
    small_storage(small_storage&&) = delete;
    small_storage& operator=(small_storage&&) = delete;

    ~small_storage() { release(); }

    T* data() noexcept { return slots_; }
    const T* data() const noexcept { return slots_; }

    constexpr size_type capacity() const noexcept { return capacity_; }

    /** @brief Whether the slots have moved off the object. */
    constexpr bool spilled() const noexcept { return capacity_ > N; }

    /**
     * @brief Allocate a block of at least `n` slots, leaving the current one live.
     *
     * Always a heap block: the returned capacity is at least `2 * capacity() > N`,
     * so it can never collide with the inline array.
     *
     * @return A falsy block when the resource could not supply the memory.
     */
    [[nodiscard]] storage_block<T> reserve_block(const size_type n) {
        const size_type target{detail::next_capacity<T>(capacity_, n)};
        assert(target > N && "small_storage: a reserved block must be larger than the inline array");

        T* ptr{detail::allocate_slots<T, Align>(resource_, target)};
        if (!ptr) {
            return {};
        }
        return {ptr, target};
    }

    /**
     * @brief Release the current block, if it was a spilled one, and take `block`.
     * @pre The container has already relocated its live elements into `block`.
     */
    void adopt(const storage_block<T> block) noexcept {
        assert(block.data != nullptr && "small_storage: cannot adopt an empty block");
        assert(block.data != inline_data() && "small_storage: cannot adopt the inline array");

        release();
        slots_ = block.data;
        capacity_ = block.capacity;
    }

    /**
     * @brief Give `block` back without adopting it; the current block stays live.
     * @pre `block` holds no live objects and came from `reserve_block`.
     */
    void discard(const storage_block<T> block) noexcept {
        assert(block.data != inline_data() && "small_storage: the inline array is not a reserved block");
        detail::free_slots<T, Align>(resource_, block.data, block.capacity);
    }

    /**
     * @brief Become a storage over `other`'s slots and resource, as far as the slots can travel.
     *
     * `other`'s resource is taken either way, spilled or not, so a block is never
     * released through a resource that did not supply it.
     *
     * @return `true` when the block itself came across, leaving `other` inline.
     *         `false` when `other` had not spilled, in which case the caller must
     *         move `other`'s elements into these inline slots; they fit, both
     *         arrays being `N` long.
     * @pre This storage holds no live elements.
     */
    [[nodiscard]] bool adopt_from(small_storage& other) noexcept {
        if (this == &other) {
            return false;
        }

        release();
        resource_ = other.resource_;

        if (!other.spilled()) {
            return false;
        }

        slots_ = std::exchange(other.slots_, other.inline_data());
        capacity_ = std::exchange(other.capacity_, N);

        return true;
    }

    /** @brief Access the backing resource. */
    constexpr auto& resource(this auto&& self) noexcept { return self.resource_; }

private:
    Resource resource_{};
    T* slots_{};
    size_type capacity_{N};

    /* Last, so the two words above stay near the front of the object. */
    alignas(Align) std::byte inline_slots_[N * sizeof(T)];

    T* inline_data() noexcept { return reinterpret_cast<T*>(inline_slots_); }

    /** @brief Free the spilled block, if any, and fall back to the inline slots. */
    void release() noexcept {
        if (spilled()) {
            detail::free_slots<T, Align>(resource_, slots_, capacity_);
        }
        slots_ = inline_data();
        capacity_ = N;
    }
};

/* ============================================================================
 * Growth helper
 * ============================================================================ */

/**
 * @brief Grow `store` to hold at least `min_capacity` slots, relocating `live` elements.
 *
 * Runs the full protocol from @ref growth for a single array: reserve, move the
 * `live` elements from the front of the old block into the front of the new one,
 * destroy the originals, adopt. A no-op returning `true` when the capacity is
 * already sufficient.
 *
 * @return `false` when the resource could not supply a larger block, in which
 *         case `store` is untouched.
 * @throws Whatever `T`'s move constructor throws, having first restored `store`
 *         to its original state. Types with a non-throwing move (the common
 *         case) take a branch with no rollback path at all.
 */
export template <growable_storage S> bool relocate_grow(S& store, const typename S::size_type min_capacity, const typename S::size_type live) {
    using T = typename S::value_type;

    assert(live <= store.capacity() && "relocate_grow: more live elements than slots");

    if (store.capacity() >= min_capacity) {
        return true;
    }

    auto block{store.reserve_block(min_capacity)};
    if (!block) {
        return false;
    }

    if (live > 0) {
        if constexpr (std::is_nothrow_move_constructible_v<T>) {
            detail::relocate_n(store.data(), live, block.data);
        } else {
            try {
                detail::relocate_n(store.data(), live, block.data);
            } catch (...) {
                /* uninitialized_move_n has already destroyed whatever it managed
                 * to construct, so the new block is raw again and the old one
                 * still holds every element. */
                store.discard(block);
                throw;
            }
        }
        std::destroy_n(store.data(), live);
    }

    store.adopt(block);
    return true;
}

/* Concept verification across the four storage kinds. */
static_assert(storage_for<inline_storage<int, 8>, int>);
static_assert(fixed_extent_storage<inline_storage<int, 8>>);
static_assert(!growable_storage<inline_storage<int, 8>>);

static_assert(storage_for<fixed_storage<int, 8>, int>);
static_assert(fixed_extent_storage<fixed_storage<int, 8>>);
static_assert(!growable_storage<fixed_storage<int, 8>>);

static_assert(storage_for<dynamic_storage<int>, int>);
static_assert(!fixed_extent_storage<dynamic_storage<int>>);
static_assert(growable_storage<dynamic_storage<int>>);
static_assert(!transferable_storage<dynamic_storage<int>>);

/* Only the resource-backed kinds can be rebuilt against their resource. An
 * inline one has none, and a clone of it needs none. */
static_assert(!resourced_storage<inline_storage<int, 8>>);
static_assert(resourced_storage<fixed_storage<int, 8>>);
static_assert(resourced_storage<dynamic_storage<int>>);
static_assert(resourced_storage<small_storage<int, 8>>);
/* The case that fails quietly if the concept is wrong: an injected reference must
 * reach the clone, or it allocates through a null referent. */
static_assert(resourced_storage<dynamic_storage<int, resource_ref<default_resource>>>);

static_assert(storage_for<small_storage<int, 8>, int>);
static_assert(!fixed_extent_storage<small_storage<int, 8>>);
static_assert(growable_storage<small_storage<int, 8>>);
static_assert(transferable_storage<small_storage<int, 8>>);
/* Not relocatable: the inline slots are the object. A container over it moves via
 * adopt_from instead, which is why the two are separate questions. */
static_assert(!small_storage<int, 8>::relocatable);
/* The inline array is really inline, and rebind keeps N as a slot count. */
static_assert(sizeof(small_storage<int, 8>) >= 8 * sizeof(int));
static_assert(small_storage<int, 8>::rebind<std::size_t>::inline_capacity == 8);

/* rebind keeps the injected resource and resets the alignment to the new type's. */
static_assert(std::same_as<dynamic_storage<int, resource_ref<default_resource>>::rebind<char>, dynamic_storage<char, resource_ref<default_resource>>>);
static_assert(dynamic_storage<std::max_align_t, default_resource, 64>::rebind<char>::alignment == alignof(char));

} // namespace libmem

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-owning-memory, cppcoreguidelines-pro-bounds-pointer-arithmetic)
