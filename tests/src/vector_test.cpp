#include <gtest/gtest.h>

import libmem;
import std;

namespace {

/* A payload that counts its own lifetime events, so a test can prove the vector
 * destroys exactly what it constructed. */
struct tracked {
    static int live;
    static int moves;
    static int copies;

    int value{};

    explicit tracked(const int v) : value{v} { ++live; }
    tracked(const tracked& other) : value{other.value} {
        ++live;
        ++copies;
    }
    tracked(tracked&& other) noexcept : value{other.value} {
        ++live;
        ++moves;
    }
    tracked& operator=(const tracked&) = default;
    tracked& operator=(tracked&&) = default;
    ~tracked() { --live; }

    static void reset() {
        live = 0;
        moves = 0;
        copies = 0;
    }
};

int tracked::live{};
int tracked::moves{};
int tracked::copies{};

/* Move-only, to prove the vector never reaches for a copy where a move will do. */
struct move_only {
    std::unique_ptr<int> value{};

    explicit move_only(const int v) : value{std::make_unique<int>(v)} {}
};

/* Counts what reaches the resource, which is how "the small buffer allocated
 * nothing" is actually checked rather than assumed. */
class counting_resource {
public:
    void* allocate(const std::size_t size) {
        ++allocations_;
        ++live_;
        return ::operator new(size);
    }

    void deallocate(void* ptr, const std::size_t size) noexcept {
        --live_;
        ::operator delete(ptr, size);
    }

    void* allocate(const std::size_t size, const std::size_t align) {
        ++allocations_;
        ++live_;
        return ::operator new(size, std::align_val_t{align});
    }

    void deallocate(void* ptr, const std::size_t size, const std::size_t align) noexcept {
        --live_;
        ::operator delete(ptr, size, std::align_val_t{align});
    }

    std::size_t allocations() const noexcept { return allocations_; }
    int outstanding() const noexcept { return live_; }

private:
    std::size_t allocations_{};
    int live_{};
};

static_assert(libmem::aligned_memory_resource<counting_resource>);

/* Fails once its budget runs out, to exercise the "storage could not make room"
 * paths that are return values here rather than exceptions. */
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

/* No default constructor, to pin the one caveat on small_vector's movability. */
class unnameable_resource {
public:
    constexpr explicit unnameable_resource(int) noexcept {}

    void* allocate(const std::size_t size) { return ::operator new(size); }
    void deallocate(void* ptr, const std::size_t size) noexcept { ::operator delete(ptr, size); }
};

static_assert(libmem::memory_resource<unnameable_resource>);

using small_counted = libmem::small_vector<int, 4, libmem::resource_ref<counting_resource>>;

/* ============================================================================
 * Type surface
 * ============================================================================ */

TEST(VectorConcepts, EachAliasReportsWhatItsStorageCanDo) {
    static_assert(libmem::vector<int>::growable);
    static_assert(libmem::small_vector<int, 4>::growable);
    static_assert(!libmem::inline_vector<int, 4>::growable);
    static_assert(!libmem::fixed_vector<int, 4>::growable);

    /* Every variant moves, and every move is noexcept, so generic code relocates
     * by moving instead of falling back to copying. */
    static_assert(std::movable<libmem::vector<int>>);
    static_assert(std::movable<libmem::fixed_vector<int, 4>>);
    static_assert(std::movable<libmem::small_vector<int, 4>>);
    static_assert(std::movable<libmem::inline_vector<int, 4>>);
    static_assert(std::is_nothrow_move_constructible_v<libmem::inline_vector<int, 4>>);

    /* `relocatable` reports the cost of that move, not whether it exists. */
    static_assert(libmem::vector<int>::relocatable);
    static_assert(libmem::fixed_vector<int, 4>::relocatable);
    static_assert(!libmem::inline_vector<int, 4>::relocatable);
    static_assert(!libmem::small_vector<int, 4>::relocatable);

    /* Copying is deleted throughout, as it is for every other libmem container. */
    static_assert(!std::copyable<libmem::vector<int>>);
    static_assert(!std::copyable<libmem::small_vector<int, 4>>);
    static_assert(!std::copyable<libmem::inline_vector<int, 4>>);

    /* A small_vector moves by adopting into a fresh storage, so it needs one it can
     * build. `relocatable` has to report that honestly rather than promising a move
     * the type does not have. */
    using unmovable = libmem::small_vector<int, 4, unnameable_resource>;
    static_assert(!std::default_initializable<unnameable_resource>);
    static_assert(!unmovable::relocatable);
    static_assert(!std::movable<unmovable>);
    static_assert(unmovable::growable, "it still grows, it just cannot be moved");
}

TEST(VectorConcepts, IsAContiguousRangeThroughConst) {
    static_assert(std::ranges::contiguous_range<libmem::vector<int>>);
    static_assert(std::ranges::contiguous_range<const libmem::vector<int>>);
    static_assert(std::contiguous_iterator<libmem::vector<int>::iterator>);
    static_assert(std::same_as<std::ranges::range_reference_t<libmem::vector<int>>, int&>);
    static_assert(std::same_as<std::ranges::range_reference_t<const libmem::vector<int>>, const int&>);
}

/* ============================================================================
 * Basic sequence behaviour
 * ============================================================================ */

TEST(Vector, GrowsGeometricallyAndKeepsItsElements) {
    libmem::vector<int> v{};

    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0u);
    EXPECT_EQ(v.capacity(), 0u);

    for (int i{}; i < 1000; ++i) {
        ASSERT_NE(v.push_back(i), nullptr) << "at " << i;
    }

    EXPECT_EQ(v.size(), 1000u);
    EXPECT_GE(v.capacity(), 1000u);
    EXPECT_EQ(v.front(), 0);
    EXPECT_EQ(v.back(), 999);

    for (int i{}; i < 1000; ++i) {
        EXPECT_EQ(v[static_cast<std::size_t>(i)], i) << "at " << i;
    }
}

TEST(Vector, PushBackReturnsAPointerToWhatItStored) {
    libmem::vector<int> v{};

    int* first{v.push_back(1)};
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(*first, 1);
    EXPECT_EQ(first, v.data());
}

TEST(Vector, EmplaceBackConstructsInPlace) {
    libmem::vector<std::pair<int, std::string>> v{};

    auto* entry{v.emplace_back(7, "seven")};
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->first, 7);
    EXPECT_EQ(entry->second, "seven");
    EXPECT_EQ(v.size(), 1u);
}

TEST(Vector, HoldsMoveOnlyPayloads) {
    libmem::vector<move_only> v{};

    for (int i{}; i < 10; ++i) {
        ASSERT_NE(v.emplace_back(i), nullptr);
    }

    EXPECT_EQ(v.size(), 10u);
    EXPECT_EQ(*v[3].value, 3);
    EXPECT_EQ(*v.back().value, 9);
}

TEST(Vector, DestroysExactlyWhatItConstructed) {
    tracked::reset();
    {
        libmem::vector<tracked> v{};
        for (int i{}; i < 100; ++i) {
            ASSERT_NE(v.emplace_back(i), nullptr);
        }
        EXPECT_EQ(tracked::live, 100) << "growth must not leave copies behind";

        v.pop_back();
        EXPECT_EQ(tracked::live, 99);

        v.clear();
        EXPECT_EQ(tracked::live, 0);
        EXPECT_EQ(v.size(), 0u);
        EXPECT_GT(v.capacity(), 0u) << "clear keeps the capacity";

        ASSERT_NE(v.emplace_back(1), nullptr);
    }
    EXPECT_EQ(tracked::live, 0) << "the destructor must destroy the rest";
}

TEST(Vector, ReserveGrowsWithoutInsertingAnything) {
    libmem::vector<int> v{};

    ASSERT_TRUE(v.reserve(500));
    EXPECT_GE(v.capacity(), 500u);
    EXPECT_EQ(v.size(), 0u);

    const int* slots{v.data()};
    for (int i{}; i < 500; ++i) {
        ASSERT_NE(v.push_back(i), nullptr);
    }
    EXPECT_EQ(v.data(), slots) << "a reserved vector must not reallocate inside its reservation";
}

TEST(Vector, TruncateDestroysTheTailAndNeedsNothingOfTheElement) {
    /* `tracked` has no default constructor, which is exactly what `resize` would
     * demand and `truncate` does not. */
    static_assert(!std::default_initializable<tracked>);

    tracked::reset();
    {
        libmem::vector<tracked> v{};
        for (int i{}; i < 5; ++i) {
            ASSERT_NE(v.emplace_back(i), nullptr);
        }

        v.truncate(2);
        EXPECT_EQ(v.size(), 2u);
        EXPECT_EQ(tracked::live, 2) << "the truncated tail must be destroyed";

        v.truncate(9);
        EXPECT_EQ(v.size(), 2u) << "truncating past the end is a no-op";
    }
    EXPECT_EQ(tracked::live, 0);
}

TEST(Vector, ResizeGrowsAndShrinks) {
    libmem::vector<int> shrinking{};
    ASSERT_TRUE(shrinking.resize(5, 1));
    ASSERT_TRUE(shrinking.resize(2));
    EXPECT_EQ(shrinking.size(), 2u);

    libmem::vector<int> filled{};
    ASSERT_TRUE(filled.resize(4, 9));
    EXPECT_EQ(filled.size(), 4u);
    EXPECT_TRUE(std::ranges::all_of(filled, [](const int v) { return v == 9; }));

    ASSERT_TRUE(filled.resize(6));
    EXPECT_EQ(filled.size(), 6u);
    EXPECT_EQ(filled[4], 0) << "resize value-initialises the new tail";
    EXPECT_EQ(filled[5], 0);
}

/* ============================================================================
 * Erase
 * ============================================================================ */

TEST(Vector, EraseClosesTheGapAndKeepsTheOrder) {
    libmem::vector<int> v{};
    for (int i{}; i < 5; ++i) {
        ASSERT_NE(v.push_back(i), nullptr);
    }

    auto* next{v.erase(v.begin() + 1)};

    EXPECT_EQ(v.size(), 4u);
    EXPECT_EQ(*next, 2) << "erase returns the element that took the slot";
    EXPECT_TRUE(std::ranges::equal(v, std::array{0, 2, 3, 4}));

    /* Sequenced deliberately: the two calls in one EXPECT_EQ are unordered, so
     * `v.end()` may be read before the erase runs. */
    auto* last{v.erase(v.end() - 1)};
    EXPECT_EQ(last, v.end()) << "erasing the last one yields end()";
    EXPECT_TRUE(std::ranges::equal(v, std::array{0, 2, 3}));
}

TEST(Vector, EraseRangeRemovesTheWholeSpan) {
    libmem::vector<int> v{};
    for (int i{}; i < 6; ++i) {
        ASSERT_NE(v.push_back(i), nullptr);
    }

    v.erase(v.begin() + 1, v.begin() + 4);

    EXPECT_EQ(v.size(), 3u);
    EXPECT_TRUE(std::ranges::equal(v, std::array{0, 4, 5}));

    /* An empty range is a no-op, not an off-by-one. */
    v.erase(v.begin() + 1, v.begin() + 1);
    EXPECT_EQ(v.size(), 3u);
}

TEST(Vector, EraseDestroysTheRemovedElements) {
    tracked::reset();
    {
        libmem::vector<tracked> v{};
        for (int i{}; i < 6; ++i) {
            ASSERT_NE(v.emplace_back(i), nullptr);
        }
        ASSERT_EQ(tracked::live, 6);

        v.erase(v.begin() + 1, v.begin() + 4);
        EXPECT_EQ(tracked::live, 3) << "three erased, three left";
        EXPECT_EQ(v[0].value, 0);
        EXPECT_EQ(v[1].value, 4);
        EXPECT_EQ(v[2].value, 5);
    }
    EXPECT_EQ(tracked::live, 0);
}

TEST(Vector, EraseUnorderedSwapsTheLastElementIn) {
    libmem::vector<int> v{};
    for (int i{}; i < 5; ++i) {
        ASSERT_NE(v.push_back(i), nullptr);
    }

    auto* next{v.erase_unordered(v.begin() + 1)};

    EXPECT_EQ(v.size(), 4u);
    EXPECT_EQ(*next, 4) << "the last element took the hole";
    EXPECT_TRUE(std::ranges::equal(v, std::array{0, 4, 2, 3}));

    /* Erasing the last element needs no swap at all. Sequenced, as above. */
    auto* last{v.erase_unordered(v.end() - 1)};
    EXPECT_EQ(last, v.end());
    EXPECT_TRUE(std::ranges::equal(v, std::array{0, 4, 2}));
}

/* ============================================================================
 * Failure is a return value
 * ============================================================================ */

TEST(InlineVector, ReportsFullInsteadOfGrowing) {
    libmem::inline_vector<int, 4> v{};

    EXPECT_EQ(v.capacity(), 4u);
    for (int i{}; i < 4; ++i) {
        ASSERT_NE(v.push_back(i), nullptr) << "at " << i;
    }

    EXPECT_EQ(v.push_back(4), nullptr) << "a fixed extent reports full rather than throwing";
    EXPECT_EQ(v.size(), 4u);
    EXPECT_FALSE(v.reserve(5));
    EXPECT_FALSE(v.resize(5));
    EXPECT_EQ(v.size(), 4u) << "a failed resize must change nothing";

    /* Room again once something comes out. */
    v.pop_back();
    EXPECT_NE(v.push_back(99), nullptr);
    EXPECT_EQ(v.back(), 99);
}

TEST(InlineVector, HoldsItsElementsInsideTheObject) {
    libmem::inline_vector<std::uint64_t, 8> v{};
    ASSERT_NE(v.push_back(1), nullptr);

    EXPECT_GE(sizeof(v), 8u * sizeof(std::uint64_t));

    const auto* base{reinterpret_cast<const std::byte*>(&v)};
    const auto* slots{reinterpret_cast<const std::byte*>(v.data())};
    EXPECT_GE(slots, base);
    EXPECT_LT(slots, base + sizeof(v));
}

TEST(Vector, ReportsFailureWhenTheResourceRunsOut) {
    libmem::vector<int, limited_resource> v{limited_resource{1}};

    ASSERT_TRUE(v.reserve(4));
    const std::size_t capacity{v.capacity()};
    for (std::size_t i{}; i < capacity; ++i) {
        ASSERT_NE(v.push_back(static_cast<int>(i)), nullptr);
    }

    EXPECT_EQ(v.push_back(0), nullptr) << "budget spent";
    EXPECT_EQ(v.size(), capacity) << "a failed push must change nothing";
    EXPECT_EQ(v.capacity(), capacity);
    EXPECT_EQ(v.back(), static_cast<int>(capacity - 1));
}

/* ============================================================================
 * small_vector: the small-buffer optimisation
 * ============================================================================ */

TEST(SmallVector, AllocatesNothingWhileItFitsInline) {
    counting_resource counter{};
    {
        small_counted v{libmem::resource_ref{counter}};

        EXPECT_EQ(v.capacity(), 4u);
        for (int i{}; i < 4; ++i) {
            ASSERT_NE(v.push_back(i), nullptr) << "at " << i;
        }

        EXPECT_EQ(counter.allocations(), 0u) << "the whole point of the small buffer";
        EXPECT_FALSE(v.storage().spilled());
        EXPECT_TRUE(std::ranges::equal(v, std::array{0, 1, 2, 3}));
    }
    EXPECT_EQ(counter.outstanding(), 0);
}

TEST(SmallVector, SpillsOnceItOutgrowsTheInlineSlots) {
    counting_resource counter{};
    {
        small_counted v{libmem::resource_ref{counter}};

        for (int i{}; i < 5; ++i) {
            ASSERT_NE(v.push_back(i), nullptr) << "at " << i;
        }

        EXPECT_EQ(counter.allocations(), 1u);
        EXPECT_TRUE(v.storage().spilled());
        EXPECT_GE(v.capacity(), 5u);
        EXPECT_TRUE(std::ranges::equal(v, std::array{0, 1, 2, 3, 4}));

        /* Once spilled it stays spilled: the storage does not know how many
         * elements are live, so shrinking back is not its call. */
        v.clear();
        EXPECT_TRUE(v.storage().spilled());
    }
    EXPECT_EQ(counter.outstanding(), 0);
}

TEST(SmallVector, MovesTheElementsWhenTheSourceHasNotSpilled) {
    tracked::reset();
    {
        libmem::small_vector<tracked, 4> from{};
        for (int i{}; i < 3; ++i) {
            ASSERT_NE(from.emplace_back(i), nullptr);
        }
        ASSERT_FALSE(from.storage().spilled());
        ASSERT_EQ(tracked::live, 3);

        tracked::moves = 0;
        libmem::small_vector<tracked, 4> to{std::move(from)};

        EXPECT_EQ(to.size(), 3u);
        EXPECT_EQ(tracked::moves, 3) << "an unspilled source has to move its elements one at a time";
        EXPECT_EQ(tracked::live, 3) << "and the source's originals must be destroyed";
        EXPECT_EQ(to[0].value, 0);
        EXPECT_EQ(to[2].value, 2);

        EXPECT_EQ(from.size(), 0u); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    }
    EXPECT_EQ(tracked::live, 0);
}

TEST(SmallVector, StealsTheBlockWhenTheSourceHasSpilled) {
    tracked::reset();
    {
        libmem::small_vector<tracked, 4> from{};
        for (int i{}; i < 8; ++i) {
            ASSERT_NE(from.emplace_back(i), nullptr);
        }
        ASSERT_TRUE(from.storage().spilled());

        const tracked* slots{from.data()};
        tracked::moves = 0;

        libmem::small_vector<tracked, 4> to{std::move(from)};

        EXPECT_EQ(to.data(), slots) << "a spilled block transfers as a pointer";
        EXPECT_EQ(tracked::moves, 0) << "no element should have been touched";
        EXPECT_EQ(to.size(), 8u);
        EXPECT_EQ(tracked::live, 8);
        EXPECT_EQ(to[7].value, 7);

        EXPECT_EQ(from.size(), 0u); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        EXPECT_FALSE(from.storage().spilled());
    }
    EXPECT_EQ(tracked::live, 0);
}

TEST(SmallVector, MoveAssignmentReleasesWhatItHeld) {
    counting_resource counter{};
    {
        small_counted from{libmem::resource_ref{counter}};
        small_counted to{libmem::resource_ref{counter}};

        for (int i{}; i < 8; ++i) {
            ASSERT_NE(from.push_back(i), nullptr);
            ASSERT_NE(to.push_back(100 + i), nullptr);
        }
        ASSERT_EQ(counter.outstanding(), 2);

        to = std::move(from);

        EXPECT_EQ(counter.outstanding(), 1) << "the destination's own block must not leak";
        EXPECT_EQ(to.size(), 8u);
        EXPECT_EQ(to[0], 0);
    }
    EXPECT_EQ(counter.outstanding(), 0);
}

TEST(SmallVector, KeepsTheSourcesResourceAfterAnInlineMove) {
    counting_resource counter{};
    {
        small_counted from{libmem::resource_ref{counter}};
        ASSERT_NE(from.push_back(1), nullptr);
        ASSERT_FALSE(from.storage().spilled());

        /* Default-constructed, so it starts with no referent at all. The move has
         * to bring the source's resource across or the first spill below would
         * allocate through a null reference. */
        small_counted to{};
        to = std::move(from);

        ASSERT_EQ(to.size(), 1u);
        for (int i{}; i < 10; ++i) {
            ASSERT_NE(to.push_back(i), nullptr) << "at " << i;
        }
        EXPECT_TRUE(to.storage().spilled());
        EXPECT_GE(counter.allocations(), 1u) << "the spill must have gone through the source's resource";
    }
    EXPECT_EQ(counter.outstanding(), 0);
}

TEST(SmallVector, AVectorMovedIntoAContainerKeepsItsElements) {
    /* The reason small_vector is movable at all: a non-movable one could not be
     * returned from a function or held by another container. */
    auto make = [](const int first) {
        libmem::small_vector<int, 4> v{};
        for (int i{}; i < 6; ++i) {
            v.push_back(first + i);
        }
        return v;
    };

    libmem::vector<libmem::small_vector<int, 4>> nested{};
    ASSERT_NE(nested.push_back(make(0)), nullptr);
    ASSERT_NE(nested.push_back(make(10)), nullptr);

    EXPECT_EQ(nested.size(), 2u);
    EXPECT_TRUE(std::ranges::equal(nested[0], std::array{0, 1, 2, 3, 4, 5}));
    EXPECT_TRUE(std::ranges::equal(nested[1], std::array{10, 11, 12, 13, 14, 15}));
}

/* ============================================================================
 * Ranges
 * ============================================================================ */

TEST(Vector, BuildsFromARangeAndFeedsTheStandardAdaptors) {
    const std::array source{1, 2, 3, 4, 5};

    auto v{source | std::ranges::to<libmem::vector<int>>()};
    EXPECT_EQ(v.size(), 5u);
    EXPECT_TRUE(std::ranges::equal(v, source));

    auto evens{v | std::views::filter([](const int x) { return x % 2 == 0; }) | std::ranges::to<libmem::small_vector<int, 4>>()};
    EXPECT_TRUE(std::ranges::equal(evens, std::array{2, 4}));

    /* Writes through, so it is a range of references and not of copies. */
    for (int& x : v) {
        x *= 10;
    }
    EXPECT_TRUE(std::ranges::equal(v, std::array{10, 20, 30, 40, 50}));

    EXPECT_EQ(std::ranges::fold_left(v, 0, std::plus{}), 150);
}

TEST(Vector, AppendRangeReportsHowManyItTook) {
    libmem::vector<int> v{};
    const std::array source{1, 2, 3};

    EXPECT_EQ(v.append_range(source), 3u);
    EXPECT_EQ(v.append_range(source), 3u);
    EXPECT_TRUE(std::ranges::equal(v, std::array{1, 2, 3, 1, 2, 3}));

    /* A fixed extent takes what fits and says so, rather than throwing. */
    libmem::inline_vector<int, 4> bounded{};
    EXPECT_EQ(bounded.append_range(std::array{1, 2, 3, 4, 5, 6}), 4u);
    EXPECT_EQ(bounded.size(), 4u);
}

TEST(Vector, IteratesBackwardsAndAsASpan) {
    libmem::vector<int> v{};
    for (int i{}; i < 4; ++i) {
        ASSERT_NE(v.push_back(i), nullptr);
    }

    EXPECT_TRUE(std::ranges::equal(std::ranges::subrange{v.rbegin(), v.rend()}, std::array{3, 2, 1, 0}));

    const std::span<int> all{v.elements()};
    EXPECT_EQ(all.size(), 4u);
    EXPECT_EQ(all.data(), v.data());
}

/* ============================================================================
 * Injected resources
 * ============================================================================ */

TEST(Vector, GrowsOutOfAnArena) {
    libmem::arena scratch{1 << 16};
    const std::size_t before{scratch.used()};

    libmem::vector<std::uint32_t, libmem::resource_ref<libmem::arena>> v{libmem::resource_ref{scratch}};
    for (std::uint32_t i{}; i < 100; ++i) {
        ASSERT_NE(v.push_back(i), nullptr);
    }

    EXPECT_GT(scratch.used(), before);
    EXPECT_EQ(v.size(), 100u);
    EXPECT_EQ(v[99], 99u);
}

TEST(FixedVector, KeepsTheExtentStaticButTheSlotsOffObject) {
    libmem::fixed_vector<std::uint64_t, 1024> v{};

    EXPECT_EQ(v.capacity(), 1024u);
    EXPECT_LT(sizeof(v), 1024u * sizeof(std::uint64_t)) << "slots must not be inline";

    ASSERT_NE(v.push_back(7), nullptr);

    /* Off-object slots, so unlike inline_vector this one moves. */
    libmem::fixed_vector<std::uint64_t, 1024> moved{std::move(v)};
    EXPECT_EQ(moved.size(), 1u);
    EXPECT_EQ(moved.front(), 7u);
}

} // namespace
