#include <gtest/gtest.h>

import libmem;
import std;

namespace {

enum class entity : std::uint32_t {
};

using map = libmem::sparse_map<entity, int>;

/* A payload whose lifetime is observable, to prove the map destroys exactly once. */
struct tracked {
    static int live;

    int value{};

    explicit tracked(const int v) : value{v} { ++live; }
    tracked(const tracked& other) : value{other.value} { ++live; }
    tracked(tracked&& other) noexcept : value{other.value} { ++live; }
    tracked& operator=(const tracked&) = default;
    tracked& operator=(tracked&&) = default;
    ~tracked() { --live; }

    static void reset() { live = 0; }
};

int tracked::live{};

/**
 * @brief An id whose copy constructor is not noexcept and throws on demand.
 *
 * `sparse_map::emplace` builds the payload before handing the key to the set, so
 * the rollback that destroys that payload is gated on the *key's* copy, not the
 * payload's. Every other id in the suite is trivially copyable, which leaves that
 * branch of the `if constexpr` discarded and so never type-checked.
 */
struct fragile_id {
    using value_type = std::uint32_t;
    static constexpr value_type null_id{std::numeric_limits<value_type>::max()};

    static int copy_budget;

    value_type value{};

    fragile_id() = default;
    constexpr explicit fragile_id(const value_type v) noexcept : value{v} {}

    /* Deliberately not noexcept. */
    fragile_id(const fragile_id& other) : value{other.value} { // NOLINT(modernize-use-equals-default)
        if (copy_budget-- <= 0) {
            throw std::runtime_error{"fragile_id"};
        }
    }

    fragile_id& operator=(const fragile_id&) = default;
    ~fragile_id() = default;

    constexpr explicit operator std::uint32_t() const noexcept { return value; }
    constexpr bool operator==(const fragile_id&) const noexcept = default;
};

int fragile_id::copy_budget{};

static_assert(libmem::regular_indexable_id<fragile_id>);
static_assert(!std::is_nothrow_copy_constructible_v<fragile_id>);

/* ============================================================================
 * Basics
 * ============================================================================ */

TEST(SparseMap, StartsEmpty) {
    const map m{};

    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), 0u);
    EXPECT_FALSE(m.contains(entity{0}));
    EXPECT_EQ(m.find(entity{0}), nullptr);
    EXPECT_TRUE(m.keys().empty());
    EXPECT_TRUE(m.values().empty());
}

TEST(SparseMap, EmplaceStoresAPayloadPerId) {
    map m{};

    const auto [first, inserted]{m.emplace(entity{7}, 70)};
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(inserted);
    EXPECT_EQ(*first, 70);

    EXPECT_TRUE(m.contains(entity{7}));
    EXPECT_EQ(*m.find(entity{7}), 70);
    EXPECT_EQ(m.at(entity{7}), 70);
    EXPECT_EQ(m.size(), 1u);
}

TEST(SparseMap, EmplaceOnAPresentIdKeepsTheExistingPayload) {
    map m{};
    m.emplace(entity{7}, 70);

    const auto [slot, inserted]{m.emplace(entity{7}, 999)};
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(inserted);
    EXPECT_EQ(*slot, 70) << "emplace must not overwrite";
    EXPECT_EQ(m.size(), 1u);
}

TEST(SparseMap, InsertOrAssignOverwrites) {
    map m{};
    m.emplace(entity{7}, 70);

    const auto [slot, inserted]{m.insert_or_assign(entity{7}, 999)};
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(inserted);
    EXPECT_EQ(*slot, 999);
    EXPECT_EQ(m.at(entity{7}), 999);
    EXPECT_EQ(m.size(), 1u);

    const auto fresh{m.insert_or_assign(entity{8}, 80)};
    EXPECT_TRUE(fresh.second);
    EXPECT_EQ(m.at(entity{8}), 80);
}

TEST(SparseMap, KeysAndValuesStayIndexAligned) {
    map m{};
    for (std::uint32_t i{}; i < 8; ++i) {
        m.emplace(entity{i * 10}, static_cast<int>(i * 100));
    }

    const auto keys{m.keys()};
    const auto values{m.values()};
    ASSERT_EQ(keys.size(), 8u);
    ASSERT_EQ(values.size(), 8u);

    for (std::size_t i{}; i < keys.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint32_t>(keys[i]) * 10, static_cast<std::uint32_t>(values[i]))
            << "key " << static_cast<std::uint32_t>(keys[i]) << " lost its payload";
    }
}

TEST(SparseMap, ZipsIntoPairs) {
    map m{};
    m.emplace(entity{1}, 10);
    m.emplace(entity{2}, 20);
    m.emplace(entity{3}, 30);

    std::vector<std::pair<std::uint32_t, int>> seen{};
    for (auto&& [id, value] : std::views::zip(m.keys(), m.values())) {
        seen.emplace_back(static_cast<std::uint32_t>(id), value);
    }

    EXPECT_EQ(seen, (std::vector<std::pair<std::uint32_t, int>>{{1, 10}, {2, 20}, {3, 30}}));
}

TEST(SparseMap, ValuesAreMutableThroughTheSpan) {
    map m{};
    m.emplace(entity{1}, 10);
    m.emplace(entity{2}, 20);

    for (int& v : m.values()) {
        v *= 2;
    }

    EXPECT_EQ(m.at(entity{1}), 20);
    EXPECT_EQ(m.at(entity{2}), 40);
}

/* ============================================================================
 * Erase
 * ============================================================================ */

TEST(SparseMap, EraseMirrorsTheKeySwapOntoThePayload) {
    map m{};
    m.emplace(entity{1}, 10);
    m.emplace(entity{2}, 20);
    m.emplace(entity{3}, 30);
    m.emplace(entity{4}, 40);

    EXPECT_TRUE(m.erase(entity{2}));

    EXPECT_EQ(m.size(), 3u);
    EXPECT_FALSE(m.contains(entity{2}));
    EXPECT_EQ(m.find(entity{2}), nullptr);

    /* Entity 4 was swapped into slot 1; its payload must have come with it. */
    EXPECT_EQ(m.at(entity{1}), 10);
    EXPECT_EQ(m.at(entity{3}), 30);
    EXPECT_EQ(m.at(entity{4}), 40);

    const auto keys{m.keys()};
    const auto values{m.values()};
    for (std::size_t i{}; i < keys.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint32_t>(keys[i]) * 10, static_cast<std::uint32_t>(values[i])) << "misaligned at " << i;
    }
}

TEST(SparseMap, ErasingTheLastEntryNeedsNoSwap) {
    map m{};
    m.emplace(entity{1}, 10);
    m.emplace(entity{2}, 20);

    EXPECT_TRUE(m.erase(entity{2}));
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(m.at(entity{1}), 10);
}

TEST(SparseMap, ErasingAnAbsentIdIsANoOp) {
    map m{};
    m.emplace(entity{1}, 10);

    EXPECT_FALSE(m.erase(entity{2}));
    EXPECT_FALSE(m.erase(entity{999999}));
    EXPECT_EQ(m.size(), 1u);
}

TEST(SparseMap, ErasingInterleavedWithInsertingKeepsEveryPayload) {
    map m{};
    for (std::uint32_t i{}; i < 200; ++i) {
        ASSERT_TRUE(m.emplace(entity{i}, static_cast<int>(i)).second) << "insert " << i;
    }

    /* Drop the even ids, then check every odd one still maps to its own payload. */
    for (std::uint32_t i{}; i < 200; i += 2) {
        ASSERT_TRUE(m.erase(entity{i})) << "erase " << i;
    }

    EXPECT_EQ(m.size(), 100u);
    for (std::uint32_t i{1}; i < 200; i += 2) {
        const int* found{m.find(entity{i})};
        ASSERT_NE(found, nullptr) << "lost " << i;
        EXPECT_EQ(*found, static_cast<int>(i)) << "wrong payload for " << i;
    }
    for (std::uint32_t i{}; i < 200; i += 2) {
        EXPECT_EQ(m.find(entity{i}), nullptr) << "stale " << i;
    }
}

/* ============================================================================
 * Payload lifetimes
 * ============================================================================ */

TEST(SparseMapLifetime, DestroysEveryPayloadExactlyOnce) {
    tracked::reset();
    {
        libmem::sparse_map<entity, tracked> m{};
        for (std::uint32_t i{}; i < 100; ++i) {
            m.emplace(entity{i}, static_cast<int>(i));
        }
        EXPECT_EQ(tracked::live, 100) << "growth must not leak or double-destroy";

        ASSERT_TRUE(m.erase(entity{50}));
        EXPECT_EQ(tracked::live, 99);

        m.clear();
        EXPECT_EQ(tracked::live, 0);
        EXPECT_TRUE(m.empty());
    }
    EXPECT_EQ(tracked::live, 0);
}

TEST(SparseMapLifetime, HandlesANonTriviallyDestructiblePayload) {
    libmem::sparse_map<entity, std::string> m{};

    m.emplace(entity{1}, "hello");
    m.emplace(entity{2}, 64, 'x');

    EXPECT_EQ(m.at(entity{1}), "hello");
    EXPECT_EQ(m.at(entity{2}).size(), 64u);

    EXPECT_TRUE(m.erase(entity{1}));
    EXPECT_EQ(m.at(entity{2}).size(), 64u) << "the swap must move the string, not corrupt it";
}

TEST(SparseMapLifetime, MoveOnlyPayloadWorks) {
    libmem::sparse_map<entity, std::unique_ptr<int>> m{};

    m.emplace(entity{1}, std::make_unique<int>(10));
    m.emplace(entity{2}, std::make_unique<int>(20));
    m.emplace(entity{3}, std::make_unique<int>(30));

    ASSERT_NE(m.find(entity{2}), nullptr);
    EXPECT_EQ(**m.find(entity{2}), 20);

    ASSERT_TRUE(m.erase(entity{1}));
    ASSERT_NE(m.find(entity{3}), nullptr);
    EXPECT_EQ(**m.find(entity{3}), 30);
    EXPECT_EQ(**m.find(entity{2}), 20);
    EXPECT_EQ(m.size(), 2u);
}

TEST(SparseMapRollback, DestroysThePayloadWhenTheKeyCopyThrows) {
    tracked::reset();
    {
        libmem::sparse_map<fragile_id, tracked> m{};

        fragile_id::copy_budget = 1000;
        ASSERT_TRUE(m.emplace(fragile_id{1}, 10).second);
        ASSERT_EQ(tracked::live, 1);

        /* The first emplace already grew both arrays, so this one reaches the key
         * copy without reallocating: the throw lands exactly where we want it. */
        fragile_id::copy_budget = 0;
        EXPECT_THROW(static_cast<void>(m.emplace(fragile_id{2}, 20)), std::runtime_error);
        fragile_id::copy_budget = 1000;

        EXPECT_EQ(tracked::live, 1) << "the payload built for the failed insert must be destroyed";
        EXPECT_EQ(m.size(), 1u);
        EXPECT_FALSE(m.contains(fragile_id{2}));
        EXPECT_EQ(m.at(fragile_id{1}).value, 10) << "the surviving entry is untouched";

        /* Still usable afterwards. */
        ASSERT_TRUE(m.emplace(fragile_id{2}, 20).second);
        EXPECT_EQ(m.at(fragile_id{2}).value, 20);
        EXPECT_EQ(tracked::live, 2);
    }
    EXPECT_EQ(tracked::live, 0);
}

/* ============================================================================
 * Growth and storage variants
 * ============================================================================ */

TEST(SparseMap, SurvivesManyGrowthsWithEveryPayloadIntact) {
    map m{};
    constexpr std::uint32_t count{3000};

    for (std::uint32_t i{}; i < count; ++i) {
        ASSERT_TRUE(m.emplace(entity{i}, static_cast<int>(i * 2)).second) << "insert " << i;
    }

    EXPECT_EQ(m.size(), count);
    for (std::uint32_t i{}; i < count; ++i) {
        const int* found{m.find(entity{i})};
        ASSERT_NE(found, nullptr) << "lost " << i;
        EXPECT_EQ(*found, static_cast<int>(i * 2));
    }
}

TEST(SparseMap, ReserveGrowsBothArrays) {
    map m{};
    ASSERT_TRUE(m.reserve(256));
    EXPECT_GE(m.capacity(), 256u);

    /* No reallocation should be needed now, so payload pointers stay put. */
    m.emplace(entity{0}, 0);
    const int* first{m.find(entity{0})};
    for (std::uint32_t i{1}; i < 200; ++i) {
        m.emplace(entity{i}, static_cast<int>(i));
    }
    EXPECT_EQ(m.find(entity{0}), first) << "reserved capacity should have avoided a regrow";
}

TEST(SparseMapFixed, InlineStorageIsFixedCapacity) {
    using fixed_map = libmem::sparse_map<entity, int, libmem::inline_storage<entity, 16>>;
    static_assert(!fixed_map::growable);
    static_assert(fixed_map::static_capacity == 16);

    fixed_map m{};
    for (std::uint32_t i{}; i < 16; ++i) {
        ASSERT_TRUE(m.emplace(entity{i}, static_cast<int>(i)).second) << "insert " << i;
    }
    EXPECT_EQ(m.size(), 16u);

    /* Full and un-growable: emplace reports failure and stores nothing. */
    const auto [slot, inserted]{m.emplace(entity{100}, 100)};
    EXPECT_EQ(slot, nullptr);
    EXPECT_FALSE(inserted);
    EXPECT_EQ(m.size(), 16u);

    for (std::uint32_t i{}; i < 16; ++i) {
        EXPECT_EQ(m.at(entity{i}), static_cast<int>(i));
    }
}

TEST(SparseMapFixed, FixedStorageIsNonGrowableButStillMovable) {
    /* The middle case: a compile-time extent like inline_storage, but heap-backed,
     * so unlike inline_storage the map can still be moved. */
    using fixed_map = libmem::sparse_map<entity, int, libmem::fixed_storage<entity, 32>>;
    static_assert(!fixed_map::growable);
    static_assert(fixed_map::relocatable);
    static_assert(std::movable<fixed_map>);
    static_assert(fixed_map::static_capacity == 32);

    fixed_map m{};
    for (std::uint32_t i{}; i < 32; ++i) {
        ASSERT_TRUE(m.emplace(entity{i}, static_cast<int>(i * 3)).second) << "insert " << i;
    }
    EXPECT_EQ(m.size(), 32u);
    EXPECT_EQ(m.emplace(entity{100}, 0).first, nullptr) << "full and un-growable";

    fixed_map moved{std::move(m)};
    EXPECT_EQ(moved.size(), 32u);
    for (std::uint32_t i{}; i < 32; ++i) {
        EXPECT_EQ(moved.at(entity{i}), static_cast<int>(i * 3));
    }

    /* Coherently empty, not a map still claiming slots it handed away. */
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.capacity(), 0u);
    EXPECT_FALSE(m.contains(entity{0}));
    EXPECT_EQ(m.find(entity{0}), nullptr);
}

TEST(SparseMapArena, GrowsOutOfAnInjectedArena) {
    libmem::arena scratch{1 << 20};
    using arena_map = libmem::sparse_map<entity, int, libmem::dynamic_storage<entity, libmem::resource_ref<libmem::arena>>>;

    arena_map m{libmem::resource_ref{scratch}};
    for (std::uint32_t i{}; i < 300; ++i) {
        ASSERT_TRUE(m.emplace(entity{i}, static_cast<int>(i)).second) << "insert " << i;
    }

    EXPECT_EQ(m.size(), 300u);
    for (std::uint32_t i{}; i < 300; ++i) {
        EXPECT_EQ(m.at(entity{i}), static_cast<int>(i));
    }
}

TEST(SparseMap, MoveTransfersEverything) {
    static_assert(std::movable<map>);

    map from{};
    from.emplace(entity{1}, 10);
    from.emplace(entity{2}, 20);

    map to{std::move(from)};

    EXPECT_EQ(to.size(), 2u);
    EXPECT_EQ(to.at(entity{1}), 10);
    EXPECT_EQ(to.at(entity{2}), 20);
    EXPECT_EQ(from.size(), 0u);

    map assigned{};
    assigned.emplace(entity{9}, 90);
    assigned = std::move(to);
    EXPECT_EQ(assigned.size(), 2u);
    EXPECT_EQ(assigned.at(entity{1}), 10);
    EXPECT_FALSE(assigned.contains(entity{9}));
}

TEST(SparseMap, ConstAccessYieldsConstPayloads) {
    map m{};
    m.emplace(entity{1}, 10);

    const map& c{m};
    static_assert(std::same_as<decltype(c.find(entity{1})), const int*>);
    static_assert(std::same_as<decltype(m.find(entity{1})), int*>);

    ASSERT_NE(c.find(entity{1}), nullptr);
    EXPECT_EQ(*c.find(entity{1}), 10);
    EXPECT_EQ(c.at(entity{1}), 10);
    EXPECT_TRUE(c.key_set_view().contains(entity{1}));
}

} // namespace
