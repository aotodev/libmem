/**
 * @file block_alignment_test.cpp
 * @brief Block geometry: `multislab` honours its `BlockAlign`, `pool<T>` honours `alignof(T)`,
 *        and neither pads a block up to a cache line.
 *
 * A block is only as aligned as the slab's backing memory, since `slab` addresses block `i`
 * as `base + i * BlockSize`. These pin that `multislab` takes that memory at `BlockAlign`, and
 * that blocks stay at the element's own stride rather than being padded.
 *
 * @note Near-useless under ASan, whose allocator aligns generously enough to hide a
 *       misalignment; run them optimised (`./scripts/make.sh --release --test`).
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

import libmem;
import std;

namespace {

/** @brief Alignment beyond what an unaligned `operator new` promises. */
struct alignas(64) cache_line_payload {
    std::uint64_t v[8];
};
static_assert(alignof(cache_line_payload) == 64);

/** @brief Alignment beyond even a cache line. */
struct alignas(128) double_cache_line_payload {
    std::uint64_t v[16];
};
static_assert(alignof(double_cache_line_payload) == 128);

/** @brief How many of `count` emplaced elements sat on an address `T` cannot legally occupy. */
template <typename T, std::uint32_t BlocksPerSlab> std::size_t count_misaligned(const std::int32_t count) {
    libmem::pool<T, BlocksPerSlab> p{};
    std::size_t misaligned{};
    for (std::int32_t i{0}; i < count; ++i) {
        const auto it = p.emplace();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): inspecting the address is the point
        if (reinterpret_cast<std::uintptr_t>(std::addressof(*it)) % alignof(T) != 0) {
            ++misaligned;
        }
    }
    return misaligned;
}

} // namespace

/* ============================================================================
 * pool<T> honours alignof(T)
 * ============================================================================ */

TEST(BlockAlignment, pool_honours_over_aligned_element) {
    EXPECT_EQ((count_misaligned<cache_line_payload, 64>(200)), 0u);
}

/* A small BlocksPerSlab means a small slab allocation, which a general-purpose malloc aligns
 * to 16 rather than to a page. The ~16 KiB default slab tends to come back page-aligned by
 * luck, which is what kept the original bug hidden. */
TEST(BlockAlignment, pool_honours_over_aligned_element_with_small_slabs) {
    EXPECT_EQ((count_misaligned<cache_line_payload, 4>(200)), 0u);
}

TEST(BlockAlignment, pool_honours_element_aligned_beyond_a_cache_line) {
    EXPECT_EQ((count_misaligned<double_cache_line_payload, 4>(200)), 0u);
}

/* Small, ordinarily-aligned elements: the case the cache-line quantum used to mask. Their
 * alignment requirement is only 4 or 8, but it still has to be met exactly. */
TEST(BlockAlignment, pool_honours_small_element_alignment) {
    EXPECT_EQ((count_misaligned<std::uint32_t, 8>(500)), 0u);
    EXPECT_EQ((count_misaligned<std::uint64_t, 8>(500)), 0u);
}

/* Writing through the elements as well: if a block were under-aligned, an optimised build is
 * entitled to use an aligned vector store here and fault. */
TEST(BlockAlignment, over_aligned_elements_are_writable) {
    libmem::pool<cache_line_payload, 4> p{};
    for (std::int32_t i{0}; i < 64; ++i) {
        const auto it = p.emplace();
        for (std::size_t j{0}; j < 8; ++j) {
            it->v[j] = static_cast<std::uint64_t>(i) * 8u + j;
        }
    }
    std::uint64_t checksum{};
    for (const auto& e : p) {
        for (const std::uint64_t v : e.v) {
            checksum += v;
        }
    }
    /* Sum of 0 .. 511. */
    EXPECT_EQ(checksum, 511u * 512u / 2u);
}

/* ============================================================================
 * multislab honours the BlockAlign it was given
 * ============================================================================ */

TEST(BlockAlignment, multislab_honours_requested_block_alignment) {
    using aligned_ms = libmem::multislab<libmem::cache_line_size, 64, libmem::default_resource, libmem::threshold_policy, libmem::cache_line_size>;
    static_assert(aligned_ms::block_alignment == libmem::cache_line_size);

    aligned_ms ms{};
    for (std::int32_t i{0}; i < 200; ++i) {
        void* blk = ms.allocate();
        ASSERT_NE(blk, nullptr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): inspecting the address is the point
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(blk) % libmem::cache_line_size, 0u);
    }
}

/* A stride that is not a power of two: block i sits at base + i * 24, so successive blocks
 * cycle through alignments and only the requested 8 is guaranteed. */
TEST(BlockAlignment, multislab_honours_non_power_of_two_stride) {
    libmem::multislab<24, 32, libmem::default_resource, libmem::threshold_policy, 8> ms{};
    for (std::int32_t i{0}; i < 200; ++i) {
        void* blk = ms.allocate();
        ASSERT_NE(blk, nullptr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): inspecting the address is the point
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(blk) % 8u, 0u);
    }
}

/* ============================================================================
 * Block geometry is the element's, not a cache line's
 * ============================================================================ */

TEST(BlockGeometry, pool_block_geometry_matches_the_element) {
    using u32_pool = libmem::pool<std::uint32_t>;
    static_assert(u32_pool::block_size == sizeof(std::uint32_t), "a block is one element wide, not one cache line");
    static_assert(u32_pool::block_alignment == alignof(std::uint32_t));

    using str_pool = libmem::pool<std::string>;
    static_assert(str_pool::block_size == sizeof(std::string));
    static_assert(str_pool::block_alignment == alignof(std::string));

    /* An over-aligned element carries its alignment through unchanged. */
    using aligned_pool = libmem::pool<cache_line_payload>;
    static_assert(aligned_pool::block_alignment == alignof(cache_line_payload));
    static_assert(aligned_pool::block_size == sizeof(cache_line_payload));
}

/* The regression this redesign exists to prevent: a 4-byte element used to occupy a 64-byte
 * block, so iterating a pool touched one cache line per element. */
TEST(BlockGeometry, small_elements_are_not_padded_to_a_cache_line) {
    static_assert(libmem::pool<std::uint32_t>::block_size < libmem::cache_line_size);
    static_assert(libmem::pool<std::uint64_t>::block_size < libmem::cache_line_size);
    static_assert(libmem::pool<std::string>::block_size < libmem::cache_line_size);

    /* 16 uint32 elements fit in one cache line's worth of blocks, rather than needing 16. */
    static_assert(libmem::cache_line_size / libmem::pool<std::uint32_t>::block_size == 16);
}

/* Blocks are contiguous at the element's own stride, so a pool page is laid out exactly like
 * an array. Checked through the addresses rather than through `block_size` alone. */
TEST(BlockGeometry, blocks_within_a_slab_are_contiguous_at_element_stride) {
    constexpr std::uint32_t per_slab{8};
    libmem::pool<std::uint64_t, per_slab> p{};

    std::vector<std::uintptr_t> addresses{};
    for (std::uint32_t i{0}; i < per_slab; ++i) {
        const auto it = p.emplace(i);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): inspecting the address is the point
        addresses.push_back(reinterpret_cast<std::uintptr_t>(std::addressof(*it)));
    }
    std::ranges::sort(addresses);

    for (std::size_t i{1}; i < addresses.size(); ++i) {
        EXPECT_EQ(addresses[i] - addresses[i - 1], sizeof(std::uint64_t)) << "blocks are not packed at sizeof(T)";
    }
}
