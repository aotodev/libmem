/**
 * @file arena_test.cpp
 * @brief Tests for `libmem::arena` — owning/borrowed buffers, alignment,
 *        emplace / push_back, exhaustion, reset, and `memory_resource` usage.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

import libmem;

using libmem::arena;

namespace {

struct trivial_vertex {
    float x{};
    float y{};
    float z{};
};
static_assert(std::is_trivially_destructible_v<trivial_vertex>);

} // namespace

TEST(ArenaTest, owning_buffer_basic_emplace) {
    arena a{1024};
    EXPECT_EQ(a.capacity(), 1024u);
    EXPECT_EQ(a.used(), 0u);
    EXPECT_TRUE(a.owns_buffer());

    auto* v = a.emplace<trivial_vertex>(1.f, 2.f, 3.f);
    ASSERT_NE(v, nullptr);
    EXPECT_FLOAT_EQ(v->x, 1.f);
    EXPECT_FLOAT_EQ(v->y, 2.f);
    EXPECT_FLOAT_EQ(v->z, 3.f);
    EXPECT_GE(a.used(), sizeof(trivial_vertex));
}

TEST(ArenaTest, borrowed_buffer) {
    alignas(std::max_align_t) std::array<std::byte, 256> buffer{};
    arena a{std::span<std::byte>{buffer}};
    EXPECT_EQ(a.capacity(), buffer.size());
    EXPECT_FALSE(a.owns_buffer());

    auto* p = a.emplace<std::uint64_t>(42u);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 42u);

    EXPECT_GE(reinterpret_cast<std::uintptr_t>(p), reinterpret_cast<std::uintptr_t>(buffer.data()));
}

TEST(ArenaTest, push_back_copy_and_move) {
    arena a{1024};
    trivial_vertex source{4.f, 5.f, 6.f};

    auto* p1 = a.push_back<trivial_vertex>(source);
    ASSERT_NE(p1, nullptr);
    EXPECT_FLOAT_EQ(p1->x, 4.f);

    auto* p2 = a.push_back<trivial_vertex>(trivial_vertex{7.f, 8.f, 9.f});
    ASSERT_NE(p2, nullptr);
    EXPECT_FLOAT_EQ(p2->y, 8.f);
}

TEST(ArenaTest, alignment_is_respected) {
    arena a{4096};

    /* Pump a 1-byte allocation first to misalign the cursor. */
    [[maybe_unused]] auto* misalign = a.allocate(1, 1);
    ASSERT_NE(misalign, nullptr);

    struct alignas(64) cache_aligned_t {
        std::uint64_t v[8];
    };
    auto* p = a.emplace<cache_aligned_t>();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(cache_aligned_t), 0u);
}

TEST(ArenaTest, exhaustion_returns_nullptr) {
    arena a{64};
    [[maybe_unused]] auto* first = a.emplace<std::array<std::byte, 64>>();
    ASSERT_NE(first, nullptr);

    auto* overflow = a.emplace<std::uint64_t>(0u);
    EXPECT_EQ(overflow, nullptr);
}

TEST(ArenaTest, reset_recycles_buffer) {
    arena a{256};

    auto* p1 = a.emplace<std::uint64_t>(123u);
    ASSERT_NE(p1, nullptr);
    const std::size_t used_before = a.used();
    EXPECT_GT(used_before, 0u);

    a.reset();
    EXPECT_EQ(a.used(), 0u);

    auto* p2 = a.emplace<std::uint64_t>(456u);

    ASSERT_NE(p2, nullptr);
    /* Same backing buffer, same first slot. */
    EXPECT_EQ(p1, p2);
    EXPECT_EQ(*p2, 456u);
}

TEST(ArenaTest, allocate_n_uninitialised) {
    arena a{1024};
    auto* xs = a.allocate_n<std::int32_t>(16);
    ASSERT_NE(xs, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(xs) % alignof(std::int32_t), 0u);

    for (std::size_t i{0}; i < 16; ++i) {
        xs[i] = static_cast<std::int32_t>(i * 3);
    }
    for (std::size_t i{0}; i < 16; ++i) {
        EXPECT_EQ(xs[i], static_cast<std::int32_t>(i * 3));
    }
}

TEST(ArenaTest, satisfies_memory_resource_concept) {
    static_assert(libmem::memory_resource<arena>);

    arena a{1024};
    void* ptr = a.allocate(256);
    ASSERT_NE(ptr, nullptr);
    /* deallocate is a no-op but must compile. */
    a.deallocate(ptr, 256);
    EXPECT_GE(a.used(), 256u);
}

TEST(ArenaTest, move_construction_transfers_ownership) {
    arena a{512};
    auto* p = a.emplace<std::uint32_t>(7u);

    ASSERT_NE(p, nullptr);

    arena b{std::move(a)};
    EXPECT_EQ(*p, 7u);
    EXPECT_EQ(b.capacity(), 512u);
    EXPECT_EQ(a.capacity(), 0u);
}
