#include <gtest/gtest.h>

import libmem;
import std;

namespace {

enum class entity : std::uint32_t {
};

/* Counts its own lifetime events, so a clone can be proven deep rather than
 * aliasing, and proven not to leak. */
struct tracked {
    static int live;
    static int copies;

    int value{};

    explicit tracked(const int v) : value{v} { ++live; }
    tracked(const tracked& other) : value{other.value} {
        ++live;
        ++copies;
    }
    tracked(tracked&& other) noexcept : value{other.value} { ++live; }
    tracked& operator=(const tracked&) = default;
    tracked& operator=(tracked&&) = default;
    ~tracked() { --live; }

    static void reset() {
        live = 0;
        copies = 0;
    }
};

int tracked::live{};
int tracked::copies{};

/* Fails once its budget runs out, so try_clone's failure path is reached. */
class limited_resource {
public:
    constexpr limited_resource() noexcept = default;
    constexpr explicit limited_resource(const int budget) noexcept : budget_{budget} {}

    void* allocate(const std::size_t size) {
        if (budget_ <= 0) {
            return nullptr;
        }
        --budget_;
        return ::operator new(size);
    }

    void deallocate(void* ptr, const std::size_t size) noexcept { ::operator delete(ptr, size); }

    void* allocate(const std::size_t size, const std::size_t align) {
        if (budget_ <= 0) {
            return nullptr;
        }
        --budget_;
        return ::operator new(size, std::align_val_t{align});
    }

    void deallocate(void* ptr, const std::size_t size, const std::size_t align) noexcept { ::operator delete(ptr, size, std::align_val_t{align}); }

private:
    int budget_{};
};

static_assert(libmem::aligned_memory_resource<limited_resource>);

/* ============================================================================
 * The rule: never implicitly copyable, always movable
 * ============================================================================ */

TEST(CopySemantics, NoContainerIsImplicitlyCopyable) {
    static_assert(!std::copyable<libmem::vector<int>>);
    static_assert(!std::copyable<libmem::small_vector<int, 4>>);
    static_assert(!std::copyable<libmem::inline_vector<int, 4>>);
    static_assert(!std::copyable<libmem::fixed_vector<int, 4>>);
    static_assert(!std::copyable<libmem::sparse_set<entity>>);
    static_assert(!std::copyable<libmem::sparse_map<entity, int>>);
    static_assert(!std::copyable<libmem::pool<int>>);
}

TEST(CopySemantics, EveryContainerMovesAndEveryMoveIsNoexcept) {
    static_assert(std::movable<libmem::vector<int>>);
    static_assert(std::movable<libmem::small_vector<int, 4>>);
    static_assert(std::movable<libmem::inline_vector<int, 4>>);
    static_assert(std::movable<libmem::fixed_vector<int, 4>>);
    static_assert(std::movable<libmem::sparse_set<entity>>);
    static_assert(std::movable<libmem::sparse_map<entity, int>>);
    static_assert(std::movable<libmem::pool<int>>);

    /* Inline-backed ones included, which is what keeps them usable in generic
     * code. A throwing move would make std::vector fall back to copying. */
    using inline_set = libmem::sparse_set<entity, libmem::inline_storage<entity, 16>>;
    static_assert(std::movable<inline_set>);
    static_assert(std::is_nothrow_move_constructible_v<inline_set>);
    static_assert(std::is_nothrow_move_constructible_v<libmem::inline_vector<int, 4>>);
    static_assert(std::is_nothrow_move_constructible_v<libmem::small_vector<int, 4>>);
}

TEST(CopySemantics, AnInlineVectorSurvivesAStdVectorReallocation) {
    /* The generic-code case the movability is for: growing a std::vector of them
     * must relocate rather than fail to compile. */
    std::vector<libmem::inline_vector<int, 4>> nested{};

    for (int i{}; i < 32; ++i) {
        libmem::inline_vector<int, 4> v{};
        ASSERT_NE(v.push_back(i), nullptr);
        ASSERT_NE(v.push_back(i * 2), nullptr);
        nested.push_back(std::move(v));
    }

    ASSERT_EQ(nested.size(), 32u);
    for (int i{}; i < 32; ++i) {
        const auto& v{nested[static_cast<std::size_t>(i)]};
        ASSERT_EQ(v.size(), 2u) << "entry " << i;
        EXPECT_EQ(v[0], i);
        EXPECT_EQ(v[1], i * 2);
    }
}

/* ============================================================================
 * basic_vector
 * ============================================================================ */

TEST(VectorClone, CloneIsDeepAndInfallibleForAFixedExtent) {
    tracked::reset();
    {
        libmem::inline_vector<tracked, 8> a{};
        for (int i{}; i < 4; ++i) {
            ASSERT_NE(a.emplace_back(i), nullptr);
        }
        tracked::copies = 0;

        libmem::inline_vector<tracked, 8> b{a.clone()};

        EXPECT_EQ(b.size(), 4u);
        EXPECT_EQ(tracked::copies, 4) << "one copy per element, no more";
        EXPECT_EQ(tracked::live, 8) << "both vectors hold their own elements";
        EXPECT_NE(b.data(), a.data()) << "a clone must not alias";

        b[0].value = 99;
        EXPECT_EQ(a[0].value, 0) << "writing through the clone must not touch the source";
    }
    EXPECT_EQ(tracked::live, 0);
}

TEST(VectorClone, TryCloneSucceedsWhenTheResourceCanSupplyIt) {
    libmem::vector<int> a{};
    for (int i{}; i < 100; ++i) {
        ASSERT_NE(a.push_back(i), nullptr);
    }

    const auto b{a.try_clone()};

    ASSERT_TRUE(b) << "an unconstrained resource must not fail";
    EXPECT_EQ(b->size(), 100u);
    EXPECT_TRUE(std::ranges::equal(*b, a));
    EXPECT_NE(b->data(), a.data());
}

TEST(VectorClone, TryCloneReportsFailureInsteadOfThrowing) {
    libmem::vector<int, limited_resource> a{limited_resource{1}};
    ASSERT_TRUE(a.reserve(4));
    for (int i{}; i < 4; ++i) {
        ASSERT_NE(a.push_back(i), nullptr);
    }

    /* The clone's storage is a copy of the source's, whose budget is spent, so the
     * clone cannot get a block. Failure comes back as a value, not an exception. */
    const auto b{a.try_clone()};

    EXPECT_FALSE(b) << "an exhausted resource must be reported, not thrown";
    EXPECT_EQ(a.size(), 4u) << "the source is untouched";
}

TEST(VectorClone, ClonesIntoTheSameResourceRatherThanADefaultOne) {
    /* The quiet failure the resourced_storage concept exists to prevent: a clone
     * built from a default-constructed storage would hold a null resource_ref and
     * assert at its first allocation instead of here. */
    static_assert(libmem::resourced_storage<libmem::dynamic_storage<int, libmem::resource_ref<libmem::arena>>>);

    libmem::arena scratch{1 << 16};
    libmem::vector<int, libmem::resource_ref<libmem::arena>> a{libmem::resource_ref{scratch}};
    for (int i{}; i < 50; ++i) {
        ASSERT_NE(a.push_back(i), nullptr);
    }

    const std::size_t before{scratch.used()};
    const auto b{a.try_clone()};

    ASSERT_TRUE(b);
    EXPECT_EQ(b->storage().resource().get(), &scratch) << "the clone must inherit the injected resource";
    EXPECT_GT(scratch.used(), before) << "and actually draw on it";
    EXPECT_TRUE(std::ranges::equal(*b, a));
}

TEST(VectorClone, ASmallVectorCloneStaysInlineWhenItFits) {
    libmem::small_vector<int, 8> a{};
    for (int i{}; i < 5; ++i) {
        ASSERT_NE(a.push_back(i), nullptr);
    }
    ASSERT_FALSE(a.storage().spilled());

    const auto b{a.try_clone()};

    ASSERT_TRUE(b);
    EXPECT_FALSE(b->storage().spilled()) << "a clone that fits inline must not allocate";
    EXPECT_TRUE(std::ranges::equal(*b, a));
}

TEST(VectorClone, AnInfallibleCloneAlsoDrawsOnTheInjectedResource) {
    /* clone()'s resourced branch, which the inline_vector test above does not
     * reach: an inline storage has no resource, so it takes the other branch. */
    libmem::arena scratch{1 << 16};
    libmem::fixed_vector<int, 64, libmem::resource_ref<libmem::arena>> a{libmem::resource_ref{scratch}};

    for (int i{}; i < 10; ++i) {
        ASSERT_NE(a.push_back(i), nullptr);
    }

    const std::size_t before{scratch.used()};
    const auto b{a.clone()};

    EXPECT_EQ(b.storage().resource().get(), &scratch) << "the clone must inherit the injected resource";
    EXPECT_GT(scratch.used(), before) << "and actually draw on it";
    EXPECT_EQ(b.size(), 10u);
    EXPECT_TRUE(std::ranges::equal(b, a));
    EXPECT_NE(b.data(), a.data());
}

TEST(VectorClone, AssignRangeReplacesTheContents) {
    libmem::vector<int> v{};
    ASSERT_EQ(v.append_range(std::array{1, 2, 3}), 3u);

    EXPECT_EQ(v.assign_range(std::array{7, 8}), 2u);
    EXPECT_TRUE(std::ranges::equal(v, std::array{7, 8})) << "assign replaces rather than appends";

    /* The clone-into-an-existing-vector form. */
    libmem::vector<int> source{};
    ASSERT_EQ(source.append_range(std::array{4, 5, 6}), 3u);
    EXPECT_EQ(v.assign_range(source), 3u);
    EXPECT_TRUE(std::ranges::equal(v, source));
}

/* ============================================================================
 * sparse_set / sparse_map
 * ============================================================================ */

TEST(SparseSetClone, CloneIsDeepAndIndependent) {
    libmem::sparse_set<entity> a{};
    for (std::uint32_t i{}; i < 20; ++i) {
        ASSERT_TRUE(a.insert(entity{i * 3}).inserted);
    }

    const auto b{a.try_clone()};
    ASSERT_TRUE(b);
    EXPECT_EQ(b->size(), 20u);

    for (std::uint32_t i{}; i < 20; ++i) {
        EXPECT_TRUE(b->contains(entity{i * 3})) << "clone lost " << (i * 3);
    }

    /* Erasing from the source must leave the clone whole. */
    a.clear();
    EXPECT_EQ(b->size(), 20u);
    EXPECT_TRUE(b->contains(entity{0}));
}

TEST(SparseSetClone, AFixedExtentSetClonesInfallibly) {
    using fixed_set = libmem::sparse_set<entity, libmem::inline_storage<entity, 32>>;

    fixed_set a{};
    for (std::uint32_t i{}; i < 32; ++i) {
        ASSERT_TRUE(a.insert(entity{i}).inserted);
    }

    const fixed_set b{a.clone()};

    EXPECT_EQ(b.size(), 32u);
    for (std::uint32_t i{}; i < 32; ++i) {
        EXPECT_TRUE(b.contains(entity{i})) << "clone lost " << i;
        EXPECT_EQ(b[b.index_of(entity{i})], entity{i});
    }
}

TEST(SparseSetClone, AnInfallibleCloneAlsoDrawsOnTheInjectedResource) {
    /* clone()'s resourced branch for the set, which the inline test above misses. */
    libmem::arena scratch{1 << 16};
    using fixed_set = libmem::sparse_set<entity, libmem::fixed_storage<entity, 32, libmem::resource_ref<libmem::arena>>>;

    fixed_set a{libmem::resource_ref{scratch}};
    for (std::uint32_t i{}; i < 12; ++i) {
        ASSERT_TRUE(a.insert(entity{i}).inserted) << "insert " << i;
    }

    const std::size_t before{scratch.used()};
    const fixed_set b{a.clone()};

    EXPECT_EQ(b.storage().resource().get(), &scratch) << "the clone must inherit the injected resource";
    EXPECT_GT(scratch.used(), before) << "and actually draw on it, for both arrays";
    EXPECT_EQ(b.size(), 12u);
    for (std::uint32_t i{}; i < 12; ++i) {
        EXPECT_TRUE(b.contains(entity{i})) << "clone lost " << i;
    }
}

TEST(SparseMapClone, ClonesKeysAndPayloadsTogether) {
    tracked::reset();
    {
        libmem::sparse_map<entity, tracked> a{};
        for (std::uint32_t i{}; i < 10; ++i) {
            ASSERT_NE(a.emplace(entity{i * 100}, static_cast<int>(i)).first, nullptr);
        }

        const auto b{a.try_clone()};
        ASSERT_TRUE(b);
        EXPECT_EQ(b->size(), 10u);
        EXPECT_EQ(tracked::live, 20) << "each map holds its own payloads";

        for (std::uint32_t i{}; i < 10; ++i) {
            const auto* found{b->find(entity{i * 100})};
            ASSERT_NE(found, nullptr) << "clone lost " << (i * 100);
            EXPECT_EQ(found->value, static_cast<int>(i));
        }

        /* Independent payloads, not shared ones. */
        a.at(entity{0}).value = 42;
        EXPECT_EQ(b->at(entity{0}).value, 0);
    }
    EXPECT_EQ(tracked::live, 0) << "neither map leaked";
}

TEST(SparseMapClone, AnInlineMapMovesByRelocatingAndLeavesTheSourceCoherent) {
    using inline_map = libmem::sparse_map<entity, tracked, libmem::inline_storage<entity, 16>>;

    static_assert(std::movable<inline_map>);
    static_assert(!inline_map::relocatable);
    static_assert(std::is_nothrow_move_constructible_v<inline_map>);

    tracked::reset();
    {
        inline_map from{};
        for (std::uint32_t i{}; i < 5; ++i) {
            ASSERT_NE(from.emplace(entity{i}, static_cast<int>(i)).first, nullptr) << "emplace " << i;
        }
        ASSERT_EQ(tracked::live, 5);

        inline_map to{std::move(from)};

        EXPECT_EQ(to.size(), 5u);
        EXPECT_EQ(tracked::live, 5) << "payloads relocated, not duplicated or leaked";
        for (std::uint32_t i{}; i < 5; ++i) {
            const auto* found{to.find(entity{i})};
            ASSERT_NE(found, nullptr) << "clone lost " << i;
            EXPECT_EQ(found->value, static_cast<int>(i)) << "payload drifted away from key " << i;
        }

        /* The ordering the move depends on: the payloads are relocated while the
         * key set still describes them, then the keys move. A source left holding
         * live subscripts would answer find() with a dangling slot. */
        EXPECT_EQ(from.size(), 0u); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        EXPECT_TRUE(from.empty());
        for (std::uint32_t i{}; i < 5; ++i) {
            EXPECT_EQ(from.find(entity{i}), nullptr) << "moved-from map still resolves " << i;
        }

        /* Reusable, not merely empty. */
        ASSERT_NE(from.emplace(entity{3}, 42).first, nullptr);
        EXPECT_EQ(from.at(entity{3}).value, 42);
        EXPECT_EQ(to.at(entity{3}).value, 3) << "the destination must be unaffected";
    }
    EXPECT_EQ(tracked::live, 0) << "neither map leaked";
}

TEST(SparseMapClone, AFixedExtentMapClonesInfallibly) {
    using inline_map = libmem::sparse_map<entity, int, libmem::inline_storage<entity, 16>>;

    inline_map a{};
    for (std::uint32_t i{}; i < 6; ++i) {
        ASSERT_NE(a.emplace(entity{i}, static_cast<int>(i * 10)).first, nullptr);
    }

    const inline_map b{a.clone()};

    EXPECT_EQ(b.size(), 6u);
    for (std::uint32_t i{}; i < 6; ++i) {
        const auto* found{b.find(entity{i})};
        ASSERT_NE(found, nullptr) << "clone lost " << i;
        EXPECT_EQ(*found, static_cast<int>(i * 10));
    }

    a.clear();
    EXPECT_EQ(b.size(), 6u) << "the clone must not alias the source";
}

TEST(SparseMapClone, APagedMapClonesToo) {
    libmem::paged_sparse_map<entity, std::string> a{};
    ASSERT_NE(a.emplace(entity{7}, "seven").first, nullptr);
    ASSERT_NE(a.emplace(entity{700'000}, "far").first, nullptr);

    const auto b{a.try_clone()};

    ASSERT_TRUE(b);
    EXPECT_EQ(b->size(), 2u);
    EXPECT_EQ(b->at(entity{7}), "seven");
    EXPECT_EQ(b->at(entity{700'000}), "far");
}

/* ============================================================================
 * pool
 * ============================================================================ */

TEST(PoolClone, ClonesElementsAndTheAllocatorConfiguration) {
    tracked::reset();
    {
        libmem::pool<tracked> a{};
        for (int i{}; i < 50; ++i) {
            a.emplace(i);
        }
        ASSERT_EQ(a.size(), 50u);

        const auto cloned{a.try_clone()};
        ASSERT_TRUE(cloned);
        const libmem::pool<tracked>& b{*cloned};

        EXPECT_EQ(b.size(), 50u);
        EXPECT_EQ(tracked::live, 100) << "each pool holds its own elements";

        /* Order is not guaranteed across a clone, so compare as multisets. */
        std::vector<int> from_source{};
        std::vector<int> from_clone{};
        for (const auto& t : a) {
            from_source.push_back(t.value);
        }
        for (const auto& t : b) {
            from_clone.push_back(t.value);
        }
        std::ranges::sort(from_source);
        std::ranges::sort(from_clone);
        EXPECT_EQ(from_source, from_clone);
    }
    EXPECT_EQ(tracked::live, 0) << "neither pool leaked";
}

TEST(PoolClone, PreservesTheSlabCap) {
    /* `slab_limit` is what disambiguates configuration from contents: `pool<int>{4}`
     * selects the initializer_list constructor and holds the element 4. */
    libmem::pool<int> a{libmem::slab_limit{4}};
    ASSERT_EQ(a.max_slabs(), 4u);
    ASSERT_EQ(a.size(), 0u) << "a cap is configuration, not an element";

    ASSERT_NE(a.emplace(1), a.end());

    const auto b{a.try_clone()};

    ASSERT_TRUE(b);
    EXPECT_EQ(b->max_slabs(), 4u) << "a clone must rebuild the whole allocator configuration, cap included";
    EXPECT_EQ(b->size(), 1u);
    EXPECT_EQ(*b->begin(), 1);
}

TEST(PoolClone, TheBracedIntegerStillMeansOneElement) {
    /* The other half of the disambiguation, pinned so a future overload cannot
     * silently steal this spelling back. */
    const libmem::pool<int> holding{4};

    EXPECT_EQ(holding.size(), 1u);
    EXPECT_EQ(*holding.begin(), 4);
    EXPECT_EQ(holding.max_slabs(), 0u) << "unlimited, since no cap was given";
}

TEST(PoolFailure, ReportsExhaustionInsteadOfAsserting) {
    /* One slab page of 8 blocks, and a hard cap of one page. */
    libmem::pool<int, 8> p{libmem::slab_limit{1}};

    for (int i{}; i < 8; ++i) {
        ASSERT_NE(p.emplace(i), p.end()) << "insert " << i;
    }
    ASSERT_EQ(p.size(), 8u);

    /* The ninth needs a second page the cap forbids. Reported, not asserted. */
    EXPECT_EQ(p.emplace(9), p.end()) << "exhaustion must come back as an end iterator";
    EXPECT_EQ(p.size(), 8u) << "a failed insert must change nothing";

    /* Still usable, and a freed slot is reused rather than needing a new page. */
    p.erase(p.begin());
    EXPECT_EQ(p.size(), 7u);
    EXPECT_NE(p.emplace(99), p.end());
    EXPECT_EQ(p.size(), 8u);
}

TEST(PoolFailure, InsertRangeStopsAtTheCapAndSaysHowMany) {
    libmem::pool<int, 8> p{libmem::slab_limit{1}};

    const auto source{std::views::iota(0, 20) | std::ranges::to<std::vector>()};
    EXPECT_EQ(p.insert_range(source), 8u) << "it must report what actually went in";
    EXPECT_EQ(p.size(), 8u);
}

TEST(PoolFailure, TryCloneReportsWhenTheCloneCannotFit) {
    libmem::pool<int, 8> a{libmem::slab_limit{1}};
    for (int i{}; i < 8; ++i) {
        ASSERT_NE(a.emplace(i), a.end());
    }

    /* The clone rebuilds the same one-page cap, so it fits exactly. */
    const auto ok{a.try_clone()};
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->size(), 8u);
}

TEST(PoolFailure, AThrowingConstructorReleasesTheBlock) {
    struct grenade {
        int value{};
        explicit grenade(const int v) : value{v} {
            if (v < 0) {
                throw std::runtime_error{"grenade"};
            }
        }
    };

    libmem::pool<grenade, 8> p{libmem::slab_limit{1}};
    for (int i{}; i < 8; ++i) {
        ASSERT_NE(p.emplace(i), p.end());
    }

    p.erase(p.begin());
    ASSERT_EQ(p.size(), 7u);

    /* One free slot. A throwing construction must hand it back, not leak it. */
    EXPECT_THROW(static_cast<void>(p.emplace(-1)), std::runtime_error);
    EXPECT_EQ(p.size(), 7u);

    EXPECT_NE(p.emplace(42), p.end()) << "the block the throw released must be available again";
    EXPECT_EQ(p.size(), 8u);
}

} // namespace
