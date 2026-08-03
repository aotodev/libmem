/**
 * @file slab_test.cpp
 * @brief Tests for `libmem::slab` — allocation, exhaustion, reuse, reset,
 *        ownership queries, and bitmap iteration.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>

import libmem;

using libmem::slab;

namespace {

constexpr std::size_t block = libmem::cache_line_size; // 64

TEST(SlabTest, allocate_until_full_then_null) {
    alignas(block) std::array<std::byte, block * 8> storage{};
    slab<block, 8> s{storage.data(), storage.size()};

    EXPECT_EQ(s.block_count(), 8u);
    EXPECT_TRUE(s.empty());

    std::array<void*, 8> blocks{};
    for (std::uint32_t i{0}; i < 8; ++i) {
        blocks[i] = s.allocate();
        ASSERT_NE(blocks[i], nullptr);
        EXPECT_TRUE(s.owns(blocks[i]));
        /* distinct from every earlier block */
        for (std::uint32_t j{0}; j < i; ++j) {
            EXPECT_NE(blocks[i], blocks[j]);
        }
    }

    EXPECT_TRUE(s.full());
    EXPECT_EQ(s.used_count(), 8u);
    EXPECT_EQ(s.allocate(), nullptr); // exhausted
}

TEST(SlabTest, deallocate_then_reuse) {
    alignas(block) std::array<std::byte, block * 4> storage{};
    slab<block, 4> s{storage.data(), storage.size()};

    std::array<void*, 4> blocks{};
    for (auto& b : blocks) {
        b = s.allocate();
        ASSERT_NE(b, nullptr);
    }
    EXPECT_EQ(s.allocate(), nullptr);

    s.deallocate(blocks[2]);
    EXPECT_EQ(s.used_count(), 3u);

    /* the lowest free bit is reused, which is the slot we just freed */
    void* reused{s.allocate()};
    EXPECT_EQ(reused, blocks[2]);
}

TEST(SlabTest, reset_frees_all_blocks) {
    alignas(block) std::array<std::byte, block * 3> storage{};
    slab<block, 3> s{storage.data(), storage.size()};

    for (std::uint32_t i{0}; i < 3; ++i) {
        ASSERT_NE(s.allocate(), nullptr);
    }
    EXPECT_TRUE(s.full());

    s.reset();
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.used_count(), 0u);

    for (std::uint32_t i{0}; i < 3; ++i) {
        ASSERT_NE(s.allocate(), nullptr);
    }
}

TEST(SlabTest, owns_distinguishes_foreign_and_unaligned_pointers) {
    alignas(block) std::array<std::byte, block * 4> storage{};
    slab<block, 4> s{storage.data(), storage.size()};

    EXPECT_TRUE(s.owns(storage.data()));
    EXPECT_TRUE(s.owns(storage.data() + block)); // second block boundary

    /* inside the region but not on a block boundary */
    EXPECT_FALSE(s.owns(storage.data() + 1));
    /* one past the end of the managed region */
    EXPECT_FALSE(s.owns(storage.data() + storage.size()));

    std::int32_t outside{};
    EXPECT_FALSE(s.owns(&outside));
}

TEST(SlabTest, iterator_visits_every_allocated_block) {
    alignas(block) std::array<std::byte, block * 8> storage{};
    slab<block, 8> s{storage.data(), storage.size()};

    for (std::uint32_t i{0}; i < 5; ++i) {
        ASSERT_NE(s.allocate(), nullptr);
    }

    const auto visited{std::ranges::distance(s.begin(), s.end())};
    EXPECT_EQ(visited, 5);
    EXPECT_EQ(static_cast<std::uint32_t>(visited), s.used_count());
}

} // namespace

/*
 * allocate_at() must agree with allocate() on which block it hands out, and
 * additionally report the bit-index that block occupies. The index is what lets
 * callers build an iterator without a second bitmap scan, so it has to line up
 * with make_iterator().
 */
TEST(SlabTest, allocate_at_reports_the_block_index) {
    constexpr std::uint32_t capacity{4};
    alignas(block) std::array<std::byte, block * capacity> storage{};
    slab<block, capacity> s{storage.data(), storage.size()};

    for (std::uint32_t expected{0}; expected < capacity; ++expected) {
        const auto alloc{s.allocate_at()};
        ASSERT_NE(alloc.ptr, nullptr);
        EXPECT_EQ(alloc.index, expected);
        /* The reported index must address the very block we were handed. */
        EXPECT_EQ(*s.make_iterator(alloc.index), alloc.ptr);
    }

    /* Exhausted: null pointer, and the index carries no meaning. */
    EXPECT_EQ(s.allocate_at().ptr, nullptr);
}

/*
 * A freed slot is reused, and allocate_at() reports the reused index — not a
 * fresh one — so iterators built from it stay consistent with the bitmap.
 */
TEST(SlabTest, allocate_at_reports_reused_index_after_deallocate) {
    constexpr std::uint32_t capacity{4};
    alignas(block) std::array<std::byte, block * capacity> storage{};
    slab<block, capacity> s{storage.data(), storage.size()};

    const auto first{s.allocate_at()};
    const auto second{s.allocate_at()};
    ASSERT_NE(first.ptr, nullptr);
    ASSERT_NE(second.ptr, nullptr);

    s.deallocate(first.ptr);

    const auto reused{s.allocate_at()};
    EXPECT_EQ(reused.ptr, first.ptr);
    EXPECT_EQ(reused.index, first.index);
}
