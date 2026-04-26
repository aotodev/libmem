/**
 * @file concepts.cppm
 * @brief Shared concepts, policies, and platform constants for libmem.
 *
 * Collects the building blocks that multiple allocators and containers depend
 * on into a single module partition:
 *
 *   - `cache_line_size`     — alignment quantum for slab blocks.
 *   - `default_alignment`   — default alignment for untyped arena allocations.
 *   - `valid_block_size`    — concept constraining slab block sizes.
 *   - `memory_resource`     — concept for injectable allocation back-ends.
 *   - `default_resource`    — `operator new` / `operator delete` resource.
 *   - `shrink_policy`       — concept for hysteresis-based slab release.
 *   - `threshold_policy`    — default shrink policy implementation.
 */
export module libmem:concepts;

import std;

namespace libmem {

/* ============================================================================
 * Platform constants
 * ============================================================================ */

/** @brief Cache-line size used for slab block alignment validation. */
export inline constexpr std::size_t cache_line_size{64};

/**
 * @brief Default alignment for raw `allocate(n)` calls — matches the strictest
 *        fundamental alignment requirement (mirrors `malloc`).
 */
export inline constexpr std::size_t default_alignment{alignof(std::max_align_t)};

/* ============================================================================
 * Block-size concept
 * ============================================================================ */

/** @brief A valid slab block size: positive and cache-line aligned. */
export template <std::size_t N>
concept valid_block_size = (N > 0) && (N % cache_line_size == 0);

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
 * @brief Default memory resource using global `operator new` / `operator delete`.
 */
export struct default_resource {
    void* allocate(const std::size_t size) { return ::operator new(size); }
    void deallocate(void* ptr, const std::size_t size) noexcept { ::operator delete(ptr, size); }
};

static_assert(memory_resource<default_resource>);

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
