/**
 * @file typed_arena_test.cpp
 * @brief Tests for `libmem::typed_arena` — non-trivially-destructible types,
 *        destructor ordering, trivial-type passthrough, and reset semantics.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

import libmem;

using libmem::typed_arena;

namespace {

/** @brief Tracks construction and destruction counts for verification. */
struct tracked {
    static inline std::int32_t alive{0};
    static inline std::int32_t destroyed{0};
    static inline std::vector<std::int32_t> destruction_order{};

    std::int32_t id{};

    explicit tracked(const std::int32_t i) noexcept : id{i} { ++alive; }
    ~tracked() {
        --alive;
        ++destroyed;
        destruction_order.push_back(id);
    }

    tracked(const tracked&) = delete;
    tracked& operator=(const tracked&) = delete;
    tracked(tracked&&) = delete;
    tracked& operator=(tracked&&) = delete;

    static void reset_counters() {
        alive = 0;
        destroyed = 0;
        destruction_order.clear();
    }
};

static_assert(!std::is_trivially_destructible_v<tracked>);

struct trivial_pod {
    std::int32_t x{};
    float y{};
};
static_assert(std::is_trivially_destructible_v<trivial_pod>);

} // namespace

TEST(TypedArenaTest, basic_emplace_nontrivial) {
    tracked::reset_counters();
    {
        typed_arena a{4096};
        auto* t1 = a.emplace<tracked>(1);
        auto* t2 = a.emplace<tracked>(2);
        auto* t3 = a.emplace<tracked>(3);

        ASSERT_NE(t1, nullptr);
        ASSERT_NE(t2, nullptr);
        ASSERT_NE(t3, nullptr);
        EXPECT_EQ(t1->id, 1);
        EXPECT_EQ(t2->id, 2);
        EXPECT_EQ(t3->id, 3);
        EXPECT_EQ(tracked::alive, 3);
    }
    /* Destructor should have been called for all three. */
    EXPECT_EQ(tracked::alive, 0);
    EXPECT_EQ(tracked::destroyed, 3);
}

TEST(TypedArenaTest, destructors_called_in_reverse_order) {
    tracked::reset_counters();
    {
        typed_arena a{4096};
        a.emplace<tracked>(10);
        a.emplace<tracked>(20);
        a.emplace<tracked>(30);
    }
    /* LIFO: 30, 20, 10 */
    ASSERT_EQ(tracked::destruction_order.size(), 3u);
    EXPECT_EQ(tracked::destruction_order[0], 30);
    EXPECT_EQ(tracked::destruction_order[1], 20);
    EXPECT_EQ(tracked::destruction_order[2], 10);
}

TEST(TypedArenaTest, reset_calls_destructors_then_recycles) {
    tracked::reset_counters();

    typed_arena a{4096};
    a.emplace<tracked>(1);
    a.emplace<tracked>(2);
    EXPECT_EQ(tracked::alive, 2);

    const std::size_t used_before = a.used();
    EXPECT_GT(used_before, 0u);

    a.reset();
    EXPECT_EQ(tracked::alive, 0);
    EXPECT_EQ(tracked::destroyed, 2);
    EXPECT_EQ(a.used(), 0u);

    /* Buffer should be reusable. */
    auto* t = a.emplace<tracked>(99);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->id, 99);
    EXPECT_EQ(tracked::alive, 1);
}

TEST(TypedArenaTest, trivially_destructible_no_overhead) {
    typed_arena a{4096};

    auto* p1 = a.emplace<trivial_pod>(42, 3.14f);
    ASSERT_NE(p1, nullptr);
    EXPECT_EQ(p1->x, 42);
    EXPECT_FLOAT_EQ(p1->y, 3.14f);

    /* For trivially destructible types, no destructor_node is allocated,
     * so used() should reflect only sizeof(trivial_pod) + alignment. */
    const std::size_t used_after_trivial = a.used();

    /* Now emplace a non-trivial type and verify more space is consumed
     * (the destructor_node adds overhead). */
    tracked::reset_counters();
    a.emplace<tracked>(1);
    const std::size_t used_after_nontrivial = a.used();

    EXPECT_GT(used_after_nontrivial - used_after_trivial, sizeof(tracked));
}

TEST(TypedArenaTest, std_string_works) {
    typed_arena a{4096};

    auto* s = a.emplace<std::string>("hello, typed_arena!");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "hello, typed_arena!");

    /* If the arena destructor doesn't call ~string, this would leak
     * (detectable with ASan). */
}

TEST(TypedArenaTest, move_construction_transfers_dtors) {
    tracked::reset_counters();
    {
        typed_arena a{4096};
        a.emplace<tracked>(1);
        a.emplace<tracked>(2);

        typed_arena b{std::move(a)};
        EXPECT_EQ(a.capacity(), 0u);
        EXPECT_EQ(b.capacity(), 4096u);

        /* No destructors called yet — ownership transferred. */
        EXPECT_EQ(tracked::alive, 2);
    }
    /* b goes out of scope — destructors should fire. */
    EXPECT_EQ(tracked::alive, 0);
    EXPECT_EQ(tracked::destroyed, 2);
}

TEST(TypedArenaTest, satisfies_memory_resource_concept) {
    static_assert(libmem::memory_resource<typed_arena>);
}
