/**
 * @file concepts.cppm
 * @brief Shared concepts, policies, and platform constants for libmem.
 *
 * Collects the building blocks that multiple allocators and containers depend
 * on into a single module partition:
 *
 *   - `cache_line_size`:     cache-line width, for keeping concurrent writers apart.
 *   - `default_alignment`:   default alignment for untyped arena allocations.
 *   - `valid_alignment`:     concept constraining an alignment argument.
 *   - `valid_block_geometry`: concept constraining a (block size, block alignment) pair.
 *   - `memory_resource`:     concept for injectable allocation back-ends.
 *   - `aligned_memory_resource`: refinement that honours an explicit alignment.
 *   - `default_resource`:    `operator new` / `operator delete` resource.
 *   - `resource_ref`:        non-owning reference to a shared resource.
 *   - `allocator_resource`:  adapts a standard Allocator to `memory_resource`.
 *   - `shrink_policy`:       concept for hysteresis-based slab release.
 *   - `threshold_policy`:    default shrink policy implementation.
 */
module;

#include <cassert>

export module libmem:concepts;

import std;

namespace libmem {

/* ============================================================================
 * Platform constants
 * ============================================================================ */

/**
 * @brief Cache-line width, for keeping data touched by different threads apart.
 *
 * 32-bit ARM (ARMv7 and earlier) uses 32-byte cache lines; everything else we
 * target uses 64. It is deliberately not bumped to 128 on Apple Silicon.
 *
 * @note Pass it as an explicit alignment where two threads write neighbouring data; it is
 *       not a general allocation quantum.
 */
export inline constexpr std::size_t cache_line_size{
#if defined(__arm__) && !defined(__aarch64__)
    32
#else
    64
#endif
};

/**
 * @brief Default alignment for raw `allocate(n)` calls; matches the strictest
 *        fundamental alignment requirement (mirrors `malloc`).
 */
export inline constexpr std::size_t default_alignment{alignof(std::max_align_t)};

/* ============================================================================
 * Alignment and block-geometry concepts
 * ============================================================================ */

/** @brief A non-zero power of two, so `/` and `%` by it are a shift and a mask. */
export template <std::size_t N>
concept power_of_two = (N > 0) && ((N & (N - 1)) == 0);

/** @brief A valid alignment argument: a non-zero power of two. */
export template <std::size_t N>
concept valid_alignment = power_of_two<N>;

/**
 * @brief A valid block geometry: `Size` is the stride between blocks, `Align` the alignment
 *        each is guaranteed; `Size` must be a whole number of `Align`s.
 *
 * For blocks holding objects of type `T` the geometry is `<sizeof(T), alignof(T)>`.
 */
export template <std::size_t Size, std::size_t Align>
concept valid_block_geometry = (Size > 0) && valid_alignment<Align> && (Size % Align == 0);

static_assert(valid_block_geometry<sizeof(std::max_align_t), alignof(std::max_align_t)>);
static_assert(valid_block_geometry<4, 4>);
static_assert(!valid_block_geometry<4, 8>);
static_assert(!valid_block_geometry<24, 16>);
static_assert(!valid_alignment<0>);
static_assert(!valid_alignment<24>);

/* ============================================================================
 * Memory resource concept & default implementation
 * ============================================================================ */

/**
 * @brief A type satisfying `memory_resource` can allocate and deallocate
 *        raw byte regions identified by size.
 */
export template <typename T>
concept memory_resource = requires(T& r, std::size_t size, void* ptr) {
    { r.allocate(size) } -> std::same_as<void*>;
    { r.deallocate(ptr, size) } -> std::same_as<void>;
};

/**
 * @brief A `memory_resource` that also honours an explicitly requested alignment.
 *
 * The plain `memory_resource` interface carries no alignment, so a resource that
 * only models it can promise no more than `default_alignment`. Over-aligned
 * storage (cache-line aligned ring slots, SIMD payloads, an over-aligned `T`)
 * therefore needs this refinement.
 *
 * @note The aligned and unaligned halves are **not** interchangeable: memory
 *       obtained from `allocate(size, align)` must be released through
 *       `deallocate(ptr, size, align)` with the same alignment.
 */
export template <typename T>
concept aligned_memory_resource = memory_resource<T> && requires(T& r, std::size_t size, std::size_t align, void* ptr) {
    { r.allocate(size, align) } -> std::same_as<void*>;
    { r.deallocate(ptr, size, align) } -> std::same_as<void>;
};

/**
 * @brief Opt-in trait marking `R` as reclaiming its memory in bulk.
 *
 * Specialise to `true` for a resource whose `deallocate` is a no-op and whose
 * memory is released wholesale, by `reset()` or by destruction, rather than one
 * allocation at a time. A bump allocator is the canonical case.
 *
 * The promise is semantic and cannot be checked: `deallocate` being a no-op has
 * the same signature as one that frees. Hence a trait rather than a `requires`
 * clause. Requiring `reset()` syntactically would be no better, because it would
 * reject `resource_ref` to an arena, which forwards allocation but not reset.
 */
export template <typename R> inline constexpr bool enable_monotonic_resource = false;

/**
 * @brief A `memory_resource` whose allocations need never be individually freed.
 *
 * The requirement of any caller that carves a fixed set of blocks once and lets
 * the resource reclaim them together. Passing a resource that does expect paired
 * deallocation, `default_resource` for instance, would leak every block.
 */
export template <typename T>
concept monotonic_resource = memory_resource<T> && enable_monotonic_resource<std::remove_cvref_t<T>>;

/** @brief A `monotonic_resource` that also honours an explicit alignment. */
export template <typename T>
concept aligned_monotonic_resource = aligned_memory_resource<T> && monotonic_resource<T>;

/**
 * @brief Default memory resource using global `operator new` / `operator delete`.
 */
export struct default_resource {
    void* allocate(const std::size_t size) { return ::operator new(size); }
    void deallocate(void* ptr, const std::size_t size) noexcept { ::operator delete(ptr, size); }

    void* allocate(const std::size_t size, const std::size_t align) { return ::operator new(size, std::align_val_t{align}); }
    void deallocate(void* ptr, const std::size_t size, const std::size_t align) noexcept { ::operator delete(ptr, size, std::align_val_t{align}); }
};

static_assert(aligned_memory_resource<default_resource>);

/* ============================================================================
 * Resource adapters
 * ============================================================================ */

/**
 * @brief Non-owning reference to a resource owned elsewhere.
 *
 * Every allocator and container in libmem stores its `Resource` **by value**, so
 * handing several of them the same `arena` needs an indirection. `resource_ref`
 * is that indirection: copyable, trivially small, and it forwards the aligned
 * overloads whenever the referent has them, so wrapping a resource never
 * silently downgrades it to `default_alignment`.
 *
 * @code
 *     libmem::arena scratch{1 << 20};
 *     libmem::pool<node, 64, libmem::resource_ref<libmem::arena>> a{libmem::resource_ref{scratch}};
 *     libmem::pool<edge, 64, libmem::resource_ref<libmem::arena>> b{libmem::resource_ref{scratch}};
 * @endcode
 *
 * @warning The referenced resource must outlive every `resource_ref` to it.
 */
export template <memory_resource R> class resource_ref {
public:
    using resource_type = R;

    /** @brief Construct an empty reference; allocating through it is a precondition violation. */
    constexpr resource_ref() noexcept = default;

    constexpr explicit resource_ref(R& resource) noexcept : resource_{std::addressof(resource)} {}

    void* allocate(const std::size_t size) {
        assert(resource_ != nullptr && "resource_ref: no referent");
        return resource_->allocate(size);
    }

    void deallocate(void* ptr, const std::size_t size) noexcept {
        assert(resource_ != nullptr && "resource_ref: no referent");
        resource_->deallocate(ptr, size);
    }

    void* allocate(const std::size_t size, const std::size_t align)
        requires aligned_memory_resource<R>
    {
        assert(resource_ != nullptr && "resource_ref: no referent");
        return resource_->allocate(size, align);
    }

    void deallocate(void* ptr, const std::size_t size, const std::size_t align) noexcept
        requires aligned_memory_resource<R>
    {
        assert(resource_ != nullptr && "resource_ref: no referent");
        resource_->deallocate(ptr, size, align);
    }

    /** @brief The referenced resource, or `nullptr` when default-constructed. */
    constexpr R* get() const noexcept { return resource_; }

    constexpr bool operator==(const resource_ref&) const noexcept = default;

private:
    R* resource_{};
};

/** @brief A reference is monotonic exactly when its referent is. */
template <typename R> inline constexpr bool enable_monotonic_resource<resource_ref<R>> = enable_monotonic_resource<R>;

static_assert(memory_resource<resource_ref<default_resource>>);
static_assert(aligned_memory_resource<resource_ref<default_resource>>);
static_assert(!monotonic_resource<default_resource>, "operator new expects a paired delete");
static_assert(!monotonic_resource<resource_ref<default_resource>>);

/**
 * @brief Adapts a standard Allocator to the `memory_resource` interface.
 *
 * Lets any `std::allocator`-conforming type (including
 * `std::pmr::polymorphic_allocator`) back a libmem allocator or container. The
 * allocator is rebound to `std::byte`, so only the byte count crosses the
 * boundary.
 *
 * @note Deliberately not an `aligned_memory_resource`: the Allocator interface
 *       cannot express a runtime alignment, so over-aligned storage will
 *       correctly refuse to instantiate against this adapter rather than
 *       quietly under-align.
 */
export template <typename Alloc> class allocator_resource {
    using byte_allocator = typename std::allocator_traits<Alloc>::template rebind_alloc<std::byte>;
    using traits = std::allocator_traits<byte_allocator>;

public:
    using allocator_type = Alloc;

    constexpr allocator_resource()
        requires std::default_initializable<byte_allocator>
    = default;

    constexpr explicit allocator_resource(const Alloc& allocator) : allocator_{allocator} {}

    void* allocate(const std::size_t size) { return traits::allocate(allocator_, size); }

    void deallocate(void* ptr, const std::size_t size) noexcept { traits::deallocate(allocator_, static_cast<std::byte*>(ptr), size); }

    /** @brief Access the rebound allocator. */
    constexpr byte_allocator& allocator() noexcept { return allocator_; }
    constexpr const byte_allocator& allocator() const noexcept { return allocator_; }

private:
    byte_allocator allocator_{};
};

static_assert(memory_resource<allocator_resource<std::allocator<int>>>);
static_assert(!aligned_memory_resource<allocator_resource<std::allocator<int>>>);

/* ============================================================================
 * Hysteresis shrink-policy concept & default implementation
 * ============================================================================ */

/** @brief Policy controlling when empty slabs are released back to the resource. */
export template <typename T>
concept shrink_policy = requires(const T& p, std::uint32_t empty_count, std::uint32_t slab_count) {
    { p.should_shrink(empty_count, slab_count) } -> std::same_as<bool>;
};

/**
 * @brief Default hysteresis policy: shrink when empty slabs exceed a
 *        configurable reserve and the pool holds more than one slab.
 */
export struct threshold_policy {
    std::uint32_t max_empty_reserve{1};

    constexpr bool should_shrink(const std::uint32_t empty_count, const std::uint32_t slab_count) const noexcept {
        return empty_count > max_empty_reserve && slab_count > 1;
    }
};

static_assert(shrink_policy<threshold_policy>);

} // namespace libmem
