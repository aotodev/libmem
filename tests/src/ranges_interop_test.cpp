#include <gtest/gtest.h>

import libmem;
import std;

/* std::ranges interop for the containers: the view adaptors they are expected to
 * feed, and std::ranges::to in both directions.
 *
 * Note that `std::ranges::to` is deliberately exercised by *calling* it rather
 * than by a `requires`-check. It is not SFINAE-friendly: a destination it cannot
 * build hard-errors from inside the instantiation, so a concept probe reports
 * "usable" for containers it actually cannot construct. Only a real call proves it.
 */

namespace {

enum class entity : std::uint32_t {
};

using set = libmem::sparse_set<entity>;
using map = libmem::sparse_map<entity, int>;

std::uint32_t raw(const entity e) {
    return static_cast<std::uint32_t>(e);
}

/* ============================================================================
 * Range concepts
 * ============================================================================ */

TEST(RangesInterop, ContainersModelTheExpectedRangeConcepts) {
    /* sparse_set iterates its dense array directly, so it is contiguous. */
    static_assert(std::ranges::contiguous_range<set>);
    static_assert(std::ranges::sized_range<set>);
    static_assert(std::ranges::common_range<set>);

    /* sparse_map zips two parallel arrays, so it is random-access but not
     * contiguous: the (id, payload) pairs do not exist in memory as pairs. */
    static_assert(std::ranges::random_access_range<map>);
    static_assert(std::ranges::sized_range<map>);
    static_assert(std::ranges::common_range<map>);
    static_assert(!std::ranges::contiguous_range<map>);

    /* pool is a node-based forward range over a sentinel. */
    static_assert(std::ranges::forward_range<libmem::pool<int>>);
}

/* ============================================================================
 * views::keys / views::values / views::elements over sparse_map
 * ============================================================================ */

TEST(RangesInterop, ViewsKeysAndValuesWorkOnSparseMapDirectly) {
    map m{};
    m.emplace(entity{1}, 10);
    m.emplace(entity{2}, 20);
    m.emplace(entity{3}, 30);

    const auto keys{m | std::views::keys | std::views::transform(raw) | std::ranges::to<std::vector>()};
    const auto values{m | std::views::values | std::ranges::to<std::vector>()};

    EXPECT_EQ(keys, (std::vector<std::uint32_t>{1, 2, 3}));
    EXPECT_EQ(values, (std::vector<int>{10, 20, 30}));

    /* views::elements<N> is the general form the other two alias. */
    const auto second{m | std::views::elements<1> | std::ranges::to<std::vector>()};
    EXPECT_EQ(second, values);
}

TEST(RangesInterop, ViewsValuesIsWritableThroughTheMap) {
    map m{};
    m.emplace(entity{1}, 10);
    m.emplace(entity{2}, 20);

    for (int& v : m | std::views::values) {
        v *= 3;
    }

    EXPECT_EQ(m.at(entity{1}), 30);
    EXPECT_EQ(m.at(entity{2}), 60);
}

TEST(RangesInterop, StructuredBindingForLoopOverSparseMap) {
    map m{};
    m.emplace(entity{7}, 70);
    m.emplace(entity{8}, 80);

    std::vector<std::pair<std::uint32_t, int>> seen{};
    for (auto&& [id, value] : m) {
        seen.emplace_back(raw(id), value);
    }

    EXPECT_EQ(seen, (std::vector<std::pair<std::uint32_t, int>>{{7, 70}, {8, 80}}));
}

TEST(RangesInterop, EachIsTheSameViewUnderAName) {
    map m{};
    m.emplace(entity{1}, 10);
    m.emplace(entity{2}, 20);

    EXPECT_EQ(std::ranges::distance(m.each()), 2);
    EXPECT_TRUE(std::ranges::equal(m.each() | std::views::values, m.values()));
}

TEST(RangesInterop, ConstSparseMapYieldsConstPayloads) {
    map m{};
    m.emplace(entity{1}, 10);

    const map& c{m};
    static_assert(std::ranges::random_access_range<const map>);

    const auto values{c | std::views::values | std::ranges::to<std::vector>()};
    EXPECT_EQ(values, (std::vector<int>{10}));

    using const_ref = std::ranges::range_reference_t<const map>;
    static_assert(std::same_as<std::tuple_element_t<1, const_ref>, const int&>);
}

/* ============================================================================
 * ranges::to, container -> standard container
 * ============================================================================ */

TEST(RangesInteropTo, SparseSetConvertsToStandardContainers) {
    set s{};
    s.insert(entity{4});
    s.insert(entity{9});
    s.insert(entity{1});

    const auto as_vector{s | std::ranges::to<std::vector>()};
    ASSERT_EQ(as_vector.size(), 3u);
    EXPECT_EQ(raw(as_vector[0]), 4u);

    const auto ids{s | std::views::transform(raw) | std::ranges::to<std::vector>()};
    EXPECT_EQ(ids, (std::vector<std::uint32_t>{4, 9, 1}));

    /* An unordered destination works too, and normalises the dense order away. */
    const auto as_set{s | std::views::transform(raw) | std::ranges::to<std::unordered_set>()};
    EXPECT_EQ(as_set, (std::unordered_set<std::uint32_t>{1, 4, 9}));
}

TEST(RangesInteropTo, SparseMapConvertsToAStandardMap) {
    map m{};
    m.emplace(entity{2}, 20);
    m.emplace(entity{5}, 50);

    const auto model{m | std::views::transform([](const auto& entry) { return std::pair{raw(std::get<0>(entry)), std::get<1>(entry)}; }) |
                     std::ranges::to<std::unordered_map<std::uint32_t, int>>()};

    ASSERT_EQ(model.size(), 2u);
    EXPECT_EQ(model.at(2u), 20);
    EXPECT_EQ(model.at(5u), 50);

    /* A range of pairs converts straight to a vector of pairs. */
    const auto pairs{m | std::ranges::to<std::vector<std::pair<entity, int>>>()};
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_EQ(raw(pairs[0].first), 2u);
    EXPECT_EQ(pairs[0].second, 20);
}

TEST(RangesInteropTo, PoolConvertsToAStandardContainer) {
    libmem::pool<int> p{};
    p.emplace(1);
    p.emplace(2);
    p.emplace(3);

    const auto as_vector{p | std::ranges::to<std::vector>()};
    EXPECT_EQ(as_vector.size(), 3u);
    EXPECT_EQ(std::ranges::fold_left(as_vector, 0, std::plus{}), 6);
}

/* ============================================================================
 * ranges::to, standard container -> our container
 * ============================================================================ */

TEST(RangesInteropTo, SparseSetIsBuildableFromARange) {
    const std::vector<entity> ids{entity{4}, entity{9}, entity{1}, entity{9}};

    const auto s{ids | std::ranges::to<set>()};

    EXPECT_EQ(s.size(), 3u) << "the duplicate is absorbed";
    EXPECT_TRUE(s.contains(entity{4}));
    EXPECT_TRUE(s.contains(entity{9}));
    EXPECT_TRUE(s.contains(entity{1}));
}

TEST(RangesInteropTo, SparseSetIsBuildableFromAPipeline) {
    const auto s{std::views::iota(0u, 20u) | std::views::filter([](const std::uint32_t i) { return i % 3 == 0; }) |
                 std::views::transform([](const std::uint32_t i) { return entity{i}; }) | std::ranges::to<set>()};

    EXPECT_EQ(s.size(), 7u); // 0 3 6 9 12 15 18
    EXPECT_TRUE(s.contains(entity{18}));
    EXPECT_FALSE(s.contains(entity{4}));
}

TEST(RangesInteropTo, SparseMapIsBuildableFromARangeOfPairs) {
    const std::vector<std::pair<entity, int>> entries{{entity{2}, 20}, {entity{5}, 50}, {entity{2}, 999}};

    const auto m{entries | std::ranges::to<map>()};

    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m.at(entity{2}), 20) << "the first entry for an id wins, as with emplace";
    EXPECT_EQ(m.at(entity{5}), 50);
}

TEST(RangesInteropTo, SparseMapIsBuildableFromAZip) {
    const std::vector<entity> ids{entity{1}, entity{2}, entity{3}};
    const std::vector<int> values{10, 20, 30};

    const auto m{std::views::zip(ids, values) | std::ranges::to<map>()};

    EXPECT_EQ(m.size(), 3u);
    EXPECT_EQ(m.at(entity{1}), 10);
    EXPECT_EQ(m.at(entity{3}), 30);
}

TEST(RangesInteropTo, RoundTripsThroughAStandardContainer) {
    map original{};
    original.emplace(entity{1}, 10);
    original.emplace(entity{4}, 40);
    original.emplace(entity{9}, 90);

    const auto copy{original | std::ranges::to<std::vector<std::pair<entity, int>>>() | std::ranges::to<map>()};

    EXPECT_EQ(copy.size(), original.size());
    for (const auto& [id, value] : original) {
        const int* found{copy.find(id)};
        ASSERT_NE(found, nullptr) << "lost " << raw(id);
        EXPECT_EQ(*found, value);
    }
}

TEST(RangesInteropTo, MoveOnlyPayloadsMoveInViaAsRvalue) {
    using ptr_map = libmem::sparse_map<entity, std::unique_ptr<int>>;

    std::vector<std::pair<entity, std::unique_ptr<int>>> entries{};
    entries.emplace_back(entity{1}, std::make_unique<int>(10));
    entries.emplace_back(entity{2}, std::make_unique<int>(20));

    /* `std::views::as_rvalue` is what makes the elements move. A bare
     * `std::move(entries)` would not: a vector's reference type stays an lvalue
     * reference however the container itself is passed, so ranges::to would try to
     * copy and a move-only payload would not compile. Standard behaviour, and the
     * same for std::vector's own from_range constructor. */
    auto m{std::move(entries) | std::views::as_rvalue | std::ranges::to<ptr_map>()};

    ASSERT_EQ(m.size(), 2u);
    ASSERT_NE(m.find(entity{1}), nullptr);
    EXPECT_EQ(**m.find(entity{1}), 10);
    EXPECT_EQ(**m.find(entity{2}), 20);
}

/* ============================================================================
 * Algorithms
 * ============================================================================ */

TEST(RangesInterop, StandardAlgorithmsApply) {
    set s{};
    for (std::uint32_t i{}; i < 10; ++i) {
        s.insert(entity{i});
    }

    EXPECT_EQ(std::ranges::count_if(s, [](const entity e) { return raw(e) % 2 == 0; }), 5);
    EXPECT_TRUE(std::ranges::any_of(s, [](const entity e) { return raw(e) == 7; }));
    EXPECT_NE(std::ranges::find(s, entity{3}), s.end());

    map m{};
    m.emplace(entity{1}, 5);
    m.emplace(entity{2}, 15);

    /* A projection over the zipped pairs. */
    const auto total{std::ranges::fold_left(m | std::views::values, 0, std::plus{})};
    EXPECT_EQ(total, 20);

    const auto biggest{std::ranges::max_element(m, {}, [](const auto& entry) { return std::get<1>(entry); })};
    ASSERT_NE(biggest, m.end());
    EXPECT_EQ(std::get<1>(*biggest), 15);
}

TEST(RangesInterop, TakeAndDropComposeOverBothContainers) {
    set s{};
    for (std::uint32_t i{}; i < 10; ++i) {
        s.insert(entity{i * 2});
    }

    const auto first_three{s | std::views::take(3) | std::views::transform(raw) | std::ranges::to<std::vector>()};
    EXPECT_EQ(first_three, (std::vector<std::uint32_t>{0, 2, 4}));

    map m{};
    for (std::uint32_t i{}; i < 5; ++i) {
        m.emplace(entity{i}, static_cast<int>(i * 10));
    }

    const auto tail{m | std::views::drop(3) | std::views::values | std::ranges::to<std::vector>()};
    EXPECT_EQ(tail, (std::vector<int>{30, 40}));

    /* Reverse needs bidirectional, which the zip provides. */
    const auto reversed{m | std::views::reverse | std::views::values | std::ranges::to<std::vector>()};
    EXPECT_EQ(reversed, (std::vector<int>{40, 30, 20, 10, 0}));
}

} // namespace
