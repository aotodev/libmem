#include <gtest/gtest.h>

import libmem;
import std;

namespace {

enum class entity : std::uint32_t {
};

/* A strong id built on a conversion operator rather than an enum. */
struct handle {
    using value_type = std::uint32_t;
    static constexpr value_type null_id{std::numeric_limits<value_type>::max()};

    value_type value{};

    constexpr explicit operator std::uint32_t() const noexcept { return value; }
    constexpr bool operator==(const handle&) const noexcept = default;
};

/* An id carrying a generation in its high bits, masked off for indexing. */
struct versioned {
    using value_type = std::uint32_t;
    static constexpr value_type null_id{std::numeric_limits<value_type>::max()};

    value_type value{};

    constexpr explicit operator std::uint32_t() const noexcept { return value; }
    constexpr std::size_t to_index() const noexcept { return value & 0xFFFFu; }
    constexpr bool operator==(const versioned&) const noexcept = default;
};

using set = libmem::sparse_set<entity>;
using fixed_set = libmem::sparse_set<entity, libmem::inline_storage<entity, 32>>;

/** @brief Every id in the set, in dense order. */
std::vector<std::uint32_t> dense_of(const set& s) {
    std::vector<std::uint32_t> out{};
    for (const entity e : s) {
        out.push_back(static_cast<std::uint32_t>(e));
    }
    return out;
}

/* ============================================================================
 * Basics
 * ============================================================================ */

TEST(SparseSet, StartsEmptyAndAllocatesNothing) {
    const set s{};

    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.capacity(), 0u);
    EXPECT_EQ(s.index_capacity(), 0u);
    EXPECT_FALSE(s.contains(entity{0}));
    EXPECT_EQ(s.index_of(entity{0}), set::npos);
    EXPECT_EQ(s.begin(), s.end());
}

TEST(SparseSet, InsertReportsThePositionAndWhetherItWasNew) {
    set s{};

    const auto first{s.insert(entity{7})};
    EXPECT_TRUE(first);
    EXPECT_TRUE(first.inserted);
    EXPECT_EQ(first.index, 0u);

    const auto again{s.insert(entity{7})};
    EXPECT_TRUE(again) << "the id is in the set either way";
    EXPECT_FALSE(again.inserted);
    EXPECT_EQ(again.index, 0u);

    EXPECT_EQ(s.size(), 1u);
}

TEST(SparseSet, ContainsAndIndexOfAgree) {
    set s{};
    s.insert(entity{5});
    s.insert(entity{100});
    s.insert(entity{0});

    EXPECT_TRUE(s.contains(entity{5}));
    EXPECT_TRUE(s.contains(entity{100}));
    EXPECT_TRUE(s.contains(entity{0}));
    EXPECT_FALSE(s.contains(entity{6}));
    EXPECT_FALSE(s.contains(entity{101}));

    EXPECT_EQ(s.index_of(entity{5}), 0u);
    EXPECT_EQ(s.index_of(entity{100}), 1u);
    EXPECT_EQ(s.index_of(entity{0}), 2u);
    EXPECT_EQ(s.index_of(entity{6}), set::npos);

    /* A lookup far past the sparse extent is a bounds check, not a read. */
    EXPECT_FALSE(s.contains(entity{999999}));
    EXPECT_EQ(s.index_of(entity{999999}), set::npos);
}

TEST(SparseSet, KeepsTheDenseArrayGapFree) {
    set s{};
    for (std::uint32_t i{}; i < 8; ++i) {
        s.insert(entity{i * 10});
    }

    EXPECT_EQ(dense_of(s), (std::vector<std::uint32_t>{0, 10, 20, 30, 40, 50, 60, 70}));
    EXPECT_EQ(s.keys().size(), 8u);
    EXPECT_EQ(static_cast<std::uint32_t>(s[3]), 30u);
}

/* ============================================================================
 * Erase
 * ============================================================================ */

TEST(SparseSet, EraseSwapsTheLastElementIntoTheHole) {
    set s{};
    s.insert(entity{1});
    s.insert(entity{2});
    s.insert(entity{3});
    s.insert(entity{4});

    const auto removed{s.erase(entity{2})};
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed.index, 1u) << "the slot 2 occupied";
    EXPECT_EQ(removed.moved_from, 3u) << "the slot 4 came from";

    EXPECT_EQ(s.size(), 3u);
    EXPECT_FALSE(s.contains(entity{2}));
    EXPECT_EQ(dense_of(s), (std::vector<std::uint32_t>{1, 4, 3}));

    /* The moved element's sparse slot must follow it. */
    EXPECT_EQ(s.index_of(entity{4}), 1u);
    EXPECT_EQ(s.index_of(entity{3}), 2u);
}

TEST(SparseSet, ErasingTheLastElementReportsNoMove) {
    set s{};
    s.insert(entity{1});
    s.insert(entity{2});

    const auto removed{s.erase(entity{2})};
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed.index, 1u);
    EXPECT_EQ(removed.moved_from, set::npos);

    EXPECT_EQ(dense_of(s), (std::vector<std::uint32_t>{1}));
}

TEST(SparseSet, ErasingAnAbsentIdIsAFalsyNoOp) {
    set s{};
    s.insert(entity{1});

    EXPECT_FALSE(s.erase(entity{2}));
    EXPECT_FALSE(s.erase(entity{999999})) << "past the sparse extent";
    EXPECT_EQ(s.size(), 1u);
    EXPECT_TRUE(s.contains(entity{1}));
}

TEST(SparseSet, ReinsertAfterEraseWorks) {
    set s{};
    s.insert(entity{1});
    s.insert(entity{2});
    ASSERT_TRUE(s.erase(entity{1}));

    EXPECT_FALSE(s.contains(entity{1}));
    const auto again{s.insert(entity{1})};
    EXPECT_TRUE(again.inserted);
    EXPECT_TRUE(s.contains(entity{1}));
    EXPECT_EQ(s.size(), 2u);
}

TEST(SparseSet, EraseDownToEmptyLeavesNoStaleMapping) {
    set s{};
    for (std::uint32_t i{}; i < 16; ++i) {
        s.insert(entity{i});
    }
    for (std::uint32_t i{}; i < 16; ++i) {
        EXPECT_TRUE(s.erase(entity{i})) << "erase " << i;
    }

    EXPECT_TRUE(s.empty());
    for (std::uint32_t i{}; i < 16; ++i) {
        EXPECT_FALSE(s.contains(entity{i})) << "stale mapping for " << i;
    }
}

TEST(SparseSet, ClearEmptiesButKeepsCapacity) {
    set s{};
    for (std::uint32_t i{}; i < 16; ++i) {
        s.insert(entity{i});
    }
    const std::size_t capacity{s.capacity()};
    const std::size_t indexed{s.index_capacity()};

    s.clear();

    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.capacity(), capacity);
    EXPECT_EQ(s.index_capacity(), indexed);
    for (std::uint32_t i{}; i < 16; ++i) {
        EXPECT_FALSE(s.contains(entity{i}));
    }

    s.insert(entity{3});
    EXPECT_EQ(s.index_of(entity{3}), 0u);
}

/* ============================================================================
 * Growth
 * ============================================================================ */

TEST(SparseSet, GrowsTheSparseArrayToTheLargestId) {
    set s{};
    s.insert(entity{1000});

    EXPECT_GE(s.index_capacity(), 1001u);
    EXPECT_TRUE(s.contains(entity{1000}));
    EXPECT_EQ(s.size(), 1u) << "one id, however large";
}

TEST(SparseSet, SurvivesManyGrowthsWithEveryIdStillFindable) {
    set s{};
    constexpr std::uint32_t count{2000};

    for (std::uint32_t i{}; i < count; ++i) {
        ASSERT_TRUE(s.insert(entity{i * 3}).inserted) << "insert " << i;
    }

    EXPECT_EQ(s.size(), count);
    for (std::uint32_t i{}; i < count; ++i) {
        EXPECT_TRUE(s.contains(entity{i * 3})) << "lost " << i;
        EXPECT_EQ(s.index_of(entity{i * 3}), i);
    }
}

TEST(SparseSet, ReserveAndReserveForAreIndependent) {
    set s{};

    ASSERT_TRUE(s.reserve(64));
    EXPECT_GE(s.capacity(), 64u);
    EXPECT_EQ(s.index_capacity(), 0u) << "reserve sizes the dense array only";

    ASSERT_TRUE(s.reserve_for(entity{500}));
    EXPECT_GE(s.index_capacity(), 501u);
    EXPECT_EQ(s.size(), 0u) << "reserving inserts nothing";
}

TEST(SparseSet, InsertRangeAddsOnlyTheNewOnes) {
    set s{};
    const std::vector<entity> ids{entity{1}, entity{2}, entity{3}, entity{2}};

    EXPECT_EQ(s.insert_range(ids), 3u);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.insert_range(ids), 0u) << "all already present";
}

/* ============================================================================
 * Ranges interop
 * ============================================================================ */

TEST(SparseSet, IsAContiguousRange) {
    static_assert(std::ranges::contiguous_range<set>);
    static_assert(std::ranges::sized_range<decltype(std::declval<const set&>().keys())>);

    set s{};
    s.insert(entity{4});
    s.insert(entity{8});
    s.insert(entity{12});

    const auto doubled{
        s | std::views::transform(libmem::to_index) | std::views::transform([](const std::size_t i) { return i * 2; }) | std::ranges::to<std::vector>()};

    EXPECT_EQ(doubled, (std::vector<std::size_t>{8, 16, 24}));
    EXPECT_EQ(std::ranges::distance(s), 3);
}

/* ============================================================================
 * Storage variants
 * ============================================================================ */

TEST(SparseSetFixed, InlineStorageAllocatesNothingAndRefusesToGrow) {
    static_assert(!fixed_set::growable);
    static_assert(fixed_set::static_capacity == 32);

    fixed_set s{};
    EXPECT_EQ(s.capacity(), 32u);
    EXPECT_EQ(s.index_capacity(), 32u) << "the rebound sparse array is N slots too";

    for (std::uint32_t i{}; i < 32; ++i) {
        EXPECT_TRUE(s.insert(entity{i}).inserted) << "insert " << i;
    }
    EXPECT_EQ(s.size(), 32u);

    /* Full: insert reports failure rather than growing. */
    const auto overflow{s.insert(entity{31})};
    EXPECT_FALSE(overflow.inserted);
    EXPECT_TRUE(overflow) << "id 31 is present, so the result is truthy";

    EXPECT_TRUE(s.contains(entity{0}));
    EXPECT_TRUE(s.contains(entity{31}));
}

TEST(SparseSetFixed, IdOutsideTheInlineExtentIsRejected) {
    fixed_set s{};

    const auto out_of_range{s.insert(entity{100})};
    EXPECT_FALSE(out_of_range) << "no sparse slot for it and no way to make one";
    EXPECT_EQ(out_of_range.index, fixed_set::npos);
    EXPECT_EQ(s.size(), 0u);
}

TEST(SparseSetFixed, FixedStorageIsNonGrowableButStillMovable) {
    /* The middle case: a compile-time extent like inline_storage, but heap-backed,
     * so unlike inline_storage the set can still be moved. */
    using fixed_heap_set = libmem::sparse_set<entity, libmem::fixed_storage<entity, 32>>;
    static_assert(!fixed_heap_set::growable);
    static_assert(fixed_heap_set::relocatable);
    static_assert(std::movable<fixed_heap_set>);

    fixed_heap_set s{};
    EXPECT_EQ(s.capacity(), 32u);
    EXPECT_EQ(s.index_capacity(), 32u);

    for (std::uint32_t i{}; i < 32; ++i) {
        ASSERT_TRUE(s.insert(entity{i}).inserted) << "insert " << i;
    }
    EXPECT_FALSE(s.insert(entity{100})) << "no sparse slot for it and no way to make one";

    fixed_heap_set moved{std::move(s)};
    EXPECT_EQ(moved.size(), 32u);
    for (std::uint32_t i{}; i < 32; ++i) {
        EXPECT_TRUE(moved.contains(entity{i})) << "lost " << i;
    }

    /* The moved-from set must be coherently empty, not a set that still claims 32
     * sparse slots it no longer owns. */
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.capacity(), 0u);
    EXPECT_EQ(s.index_capacity(), 0u);
    EXPECT_FALSE(s.contains(entity{0}));
    EXPECT_FALSE(s.insert(entity{0})) << "no slots and no way to get any";
}

TEST(SparseSetArena, GrowsOutOfAnInjectedArena) {
    libmem::arena scratch{1 << 20};
    using arena_set = libmem::sparse_set<entity, libmem::dynamic_storage<entity, libmem::resource_ref<libmem::arena>>>;

    arena_set s{libmem::resource_ref{scratch}};
    for (std::uint32_t i{}; i < 500; ++i) {
        ASSERT_TRUE(s.insert(entity{i}).inserted) << "insert " << i;
    }

    EXPECT_EQ(s.size(), 500u);
    EXPECT_GT(scratch.used(), 0u) << "both arrays came out of the arena";
    for (std::uint32_t i{}; i < 500; ++i) {
        EXPECT_TRUE(s.contains(entity{i}));
    }
}

TEST(SparseSet, MoveTransfersEverything) {
    static_assert(std::movable<set>);
    /* Inline slots are the object's own bytes, so they cannot relocate. */
    static_assert(!std::movable<fixed_set>);

    set from{};
    from.insert(entity{1});
    from.insert(entity{2});

    set to{std::move(from)};

    EXPECT_EQ(to.size(), 2u);
    EXPECT_TRUE(to.contains(entity{1}));
    EXPECT_TRUE(to.contains(entity{2}));
    EXPECT_EQ(from.size(), 0u);

    set assigned{};
    assigned.insert(entity{9});
    assigned = std::move(to);
    EXPECT_EQ(assigned.size(), 2u);
    EXPECT_TRUE(assigned.contains(entity{1}));
    EXPECT_FALSE(assigned.contains(entity{9})) << "the old contents are gone";
}

/* ============================================================================
 * Id shapes
 * ============================================================================ */

TEST(SparseSet, WorksWithPlainUnsignedAndStrongIds) {
    libmem::sparse_set<std::uint32_t> plain{};
    plain.insert(3u);
    EXPECT_TRUE(plain.contains(3u));

    libmem::sparse_set<handle> handles{};
    handles.insert(handle{11});
    EXPECT_TRUE(handles.contains(handle{11}));
    EXPECT_FALSE(handles.contains(handle{12}));
}

TEST(SparseSet, IndexesByToIndexNotByTheRawValue) {
    /* `versioned` packs a generation into its high bits: the sparse array must be
     * sized by the masked index, not by the packed value. */
    libmem::sparse_set<versioned> s{};
    s.insert(versioned{(7u << 16) | 5u});

    EXPECT_LT(s.index_capacity(), 1u << 16) << "sized by the masked index, not the packed value";
    EXPECT_TRUE(s.contains(versioned{(7u << 16) | 5u}));
}

} // namespace
