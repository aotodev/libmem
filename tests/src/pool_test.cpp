/**
 * @file pool_test.cpp
 * @brief Smoke tests for `libmem::pool` — insertion stability,
 *        erasure, iteration, and ranges interop.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <vector>

import libmem;

using libmem::pool;

namespace {

struct payload {
    std::int32_t a{};
    double b{};

    payload() = default;
    payload(const std::int32_t aa, const double bb) noexcept : a{aa}, b{bb} {}
};

} // namespace

TEST(PoolTest, empty_default_construction) {
    pool<std::int32_t> p{};
    EXPECT_TRUE(p.empty());
    EXPECT_EQ(p.size(), 0u);
    EXPECT_EQ(p.slab_count(), 0u);
    EXPECT_EQ(p.begin(), p.end());
}

TEST(PoolTest, emplace_and_iterate) {
    pool<std::int32_t> p{};
    const auto it1 = p.emplace(10);
    const auto it2 = p.emplace(20);
    const auto it3 = p.emplace(30);

    EXPECT_EQ(p.size(), 3u);
    EXPECT_EQ(*it1, 10);
    EXPECT_EQ(*it2, 20);
    EXPECT_EQ(*it3, 30);

    std::vector<std::int32_t> seen{};
    for (const std::int32_t v : p) {
        seen.push_back(v);
    }
    std::ranges::sort(seen);
    EXPECT_EQ(seen, (std::vector<std::int32_t>{10, 20, 30}));
}

TEST(PoolTest, pointer_stability) {
    pool<payload> p{};
    payload* p1 = &*p.emplace(1, 1.5);
    payload* p2 = &*p.emplace(2, 2.5);

    /* Insert many elements to force at least one new slab. */
    for (std::int32_t i{0}; i < 1024; ++i) {
        p.emplace(i, static_cast<double>(i));
    }

    EXPECT_EQ(p1->a, 1);
    EXPECT_DOUBLE_EQ(p1->b, 1.5);
    EXPECT_EQ(p2->a, 2);
    EXPECT_DOUBLE_EQ(p2->b, 2.5);
}

TEST(PoolTest, erase_returns_next) {
    pool<std::int32_t> p{};
    p.emplace(1);
    const auto e2 = p.emplace(2);
    p.emplace(3);

    const auto next = p.erase(e2);
    EXPECT_EQ(p.size(), 2u);

    std::vector<std::int32_t> seen{};
    for (const std::int32_t v : p) {
        seen.push_back(v);
    }
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_TRUE(std::ranges::find(seen, 2) == seen.end());

    /* `next` should reference one of the live elements. */
    if (next != p.end()) {
        const std::int32_t v{*next};
        EXPECT_TRUE(v == 1 || v == 3);
    }
}

TEST(PoolTest, ranges_pipe_views) {
    pool<std::int32_t> p{};
    for (std::int32_t i{1}; i <= 6; ++i) {
        p.emplace(i);
    }

    auto evens = p | std::views::filter([](const std::int32_t v) { return v % 2 == 0; });
    std::vector<std::int32_t> seen{};
    for (const std::int32_t v : evens) {
        seen.push_back(v);
    }
    std::ranges::sort(seen);
    EXPECT_EQ(seen, (std::vector<std::int32_t>{2, 4, 6}));
}

TEST(PoolTest, clear_resets_state) {
    pool<std::int32_t> p{};
    for (std::int32_t i{0}; i < 100; ++i) {
        p.emplace(i);
    }
    EXPECT_GE(p.slab_count(), 1u);
    p.clear();
    EXPECT_TRUE(p.empty());
    EXPECT_EQ(p.slab_count(), 0u);
}

TEST(PoolTest, range_construction) {
    const std::vector<std::int32_t> src{5, 10, 15, 20};
    pool<std::int32_t> p{src};
    EXPECT_EQ(p.size(), src.size());

    std::int32_t sum{};
    for (const std::int32_t v : p) {
        sum += v;
    }
    EXPECT_EQ(sum, 50);
}

TEST(PoolTest, move_construction_and_assignment) {
    pool<std::int32_t> a{};
    for (std::int32_t i{0}; i < 8; ++i) {
        a.emplace(i);
    }

    pool<std::int32_t> b{std::move(a)};
    EXPECT_EQ(b.size(), 8u);
    EXPECT_TRUE(a.empty());

    pool<std::int32_t> c{};
    c = std::move(b);
    EXPECT_EQ(c.size(), 8u);
    EXPECT_TRUE(b.empty());
}

/*
 * Stress test: insert enough elements to force the underlying multislab to
 * spawn at least two slab pages, then verify size, iteration, and that all
 * existing pointers survive growth.
 */
TEST(PoolTest, spawn_multiple_slabs) {
    using pool_t = pool<std::int32_t>;
    constexpr std::int32_t n{static_cast<std::int32_t>(pool_t::blocks_per_slab) * 3 + 7};

    pool_t p{};

    /* Track a handful of pointers from the first slab and check stability
     * after enough growth to span multiple slab pages. */
    std::vector<std::int32_t*> pinned{};
    for (std::int32_t i{0}; i < n; ++i) {
        auto it = p.emplace(i);
        if (i < 4) {
            pinned.push_back(&*it);
        }
    }

    EXPECT_EQ(p.size(), static_cast<std::size_t>(n));
    EXPECT_GE(p.slab_count(), 2u);

    for (std::size_t i{0}; i < pinned.size(); ++i) {
        EXPECT_EQ(*pinned[i], static_cast<std::int32_t>(i));
    }

    std::int64_t sum{};
    std::size_t seen{};
    for (const std::int32_t v : p) {
        sum += v;
        ++seen;
    }
    EXPECT_EQ(seen, static_cast<std::size_t>(n));

    /* Sum of 0..n-1 equals n * (n - 1) / 2. */
    const std::int64_t expected_sum{static_cast<std::int64_t>(n) * (n - 1) / 2};
    EXPECT_EQ(sum, expected_sum);
}

/*
 * Insert / erase / re-insert cycle. Verifies the freed slot is later reused
 * by a subsequent insertion (slot reuse is the whole point of a pool).
 */
TEST(PoolTest, erase_then_insert_reuses_slot) {
    pool<std::int32_t> p{};
    const auto a = p.emplace(100);
    const auto b = p.emplace(200);
    const auto c = p.emplace(300);

    std::int32_t* erased_addr = &*b;

    p.erase(b);
    EXPECT_EQ(p.size(), 2u);
    EXPECT_EQ(*a, 100);
    EXPECT_EQ(*c, 300);

    /* The new emplace should land in the freed slot (slab::allocate returns
     * the first free bit, which is the one we just cleared). */
    const auto d = p.emplace(999);
    EXPECT_EQ(p.size(), 3u);
    EXPECT_EQ(&*d, erased_addr);
    EXPECT_EQ(*d, 999);
    EXPECT_EQ(*a, 100);
    EXPECT_EQ(*c, 300);
}

/*
 * Insert N elements, erase every other one, verify counts and contents.
 */
TEST(PoolTest, erase_half_then_iterate) {
    pool<std::int32_t> p{};
    constexpr std::int32_t n{200};

    std::vector<pool<std::int32_t>::iterator> handles{};
    handles.reserve(n);
    for (std::int32_t i{0}; i < n; ++i) {
        handles.push_back(p.emplace(i));
    }

    /* Erase every even-valued element. */
    std::size_t erased{};
    for (std::int32_t i{0}; i < n; i += 2) {
        p.erase(handles[static_cast<std::size_t>(i)]);
        ++erased;
    }
    EXPECT_EQ(p.size(), static_cast<std::size_t>(n) - erased);

    /* Remaining elements must all be odd. */
    std::size_t seen{};
    for (const std::int32_t v : p) {
        EXPECT_EQ(v % 2, 1);
        ++seen;
    }
    EXPECT_EQ(seen, static_cast<std::size_t>(n) - erased);
}
