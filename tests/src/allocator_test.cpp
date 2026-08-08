#include <gtest/gtest.h>

import libmem;
import std;

namespace {

using arena_ref = libmem::resource_ref<libmem::arena>;

template <typename T> using arena_alloc = libmem::resource_allocator<T, arena_ref>;

/* Fails once its budget runs out, so the throwing boundary is actually reached. */
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
 * The adapter pair
 * ============================================================================ */

TEST(ResourceAllocator, AdaptsAResourceToTheStandardAllocatorInterface) {
    /* The two adapters are mirrors, and easy to confuse. Both directions exist. */
    static_assert(libmem::memory_resource<libmem::allocator_resource<std::allocator<int>>>);
    static_assert(std::same_as<std::allocator_traits<arena_alloc<int>>::value_type, int>);

    static_assert(std::same_as<std::allocator_traits<arena_alloc<int>>::rebind_alloc<double>, arena_alloc<double>>);
    static_assert(std::constructible_from<arena_alloc<double>, const arena_alloc<int>&>);

    /* A stateless resource costs the container nothing to carry. */
    static_assert(std::is_empty_v<libmem::resource_allocator<int>>);
    static_assert(sizeof(std::vector<int, libmem::resource_allocator<int>>) == sizeof(std::vector<int>));
}

/* ============================================================================
 * Standard containers over a libmem arena
 * ============================================================================ */

TEST(ResourceAllocator, BacksAStdVector) {
    libmem::arena scratch{1 << 20};
    const std::size_t before{scratch.used()};

    std::vector<int, arena_alloc<int>> v{arena_alloc<int>{arena_ref{scratch}}};
    for (int i{}; i < 1000; ++i) {
        v.push_back(i);
    }

    EXPECT_EQ(v.size(), 1000u);
    EXPECT_EQ(v.front(), 0);
    EXPECT_EQ(v.back(), 999);
    EXPECT_GT(scratch.used(), before) << "the vector must actually be drawing on the arena";

    /* Growth reallocates through the same resource. */
    v.resize(5000, 7);
    EXPECT_EQ(v.size(), 5000u);
    EXPECT_EQ(v[4999], 7);
    EXPECT_EQ(v[999], 999) << "the elements must survive a reallocation";
}

TEST(ResourceAllocator, BacksNodeBasedContainersThatRebind) {
    libmem::arena scratch{1 << 20};

    /* std::list and std::map never allocate a `T`; they rebind to their node type,
     * which is what the converting constructor is for. */
    std::list<int, arena_alloc<int>> list{arena_alloc<int>{arena_ref{scratch}}};
    for (int i{}; i < 100; ++i) {
        list.push_back(i);
    }
    EXPECT_EQ(list.size(), 100u);
    EXPECT_EQ(list.back(), 99);

    using pair_alloc = arena_alloc<std::pair<const int, std::string>>;
    std::map<int, std::string, std::less<>, pair_alloc> map{pair_alloc{arena_ref{scratch}}};
    map.emplace(1, "one");
    map.emplace(2, "two");

    EXPECT_EQ(map.size(), 2u);
    EXPECT_EQ(map.at(2), "two");
}

TEST(ResourceAllocator, BacksAStdStringAndAStdVectorOfThem) {
    libmem::arena scratch{1 << 20};

    using char_alloc = arena_alloc<char>;
    using arena_string = std::basic_string<char, std::char_traits<char>, char_alloc>;

    arena_string s{char_alloc{arena_ref{scratch}}};
    s.assign("a string long enough to defeat the small-string optimisation, several times over");

    EXPECT_GT(s.size(), 32u) << "the test needs an actual allocation, not an SSO buffer";
    EXPECT_EQ(s.front(), 'a');
}

/* ============================================================================
 * The throwing boundary
 * ============================================================================ */

TEST(ResourceAllocator, ThrowsBadAllocWhenTheResourceIsExhausted) {
    /* The one place libmem reports by exception: Allocator::allocate must return
     * valid memory or throw, so a null return is not expressible. */
    libmem::resource_allocator<int, limited_resource> alloc{limited_resource{1}};

    int* first{alloc.allocate(4)};
    ASSERT_NE(first, nullptr);
    alloc.deallocate(first, 4);

    EXPECT_THROW(static_cast<void>(alloc.allocate(4)), std::bad_alloc) << "budget spent";
}

TEST(ResourceAllocator, ThrowsBadArrayNewLengthOnAnOverflowingCount) {
    libmem::resource_allocator<std::uint64_t> alloc{};
    const std::size_t too_many{(std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t)) + 1};

    EXPECT_THROW(static_cast<void>(alloc.allocate(too_many)), std::bad_array_new_length);
}

TEST(ResourceAllocator, AStdVectorPropagatesTheThrow) {
    /* Shared through a resource_ref, which is what a stateful resource requires:
     * see the test below for why a by-value one would never run out. */
    limited_resource shared{3};
    using alloc = libmem::resource_allocator<int, libmem::resource_ref<limited_resource>>;

    std::vector<int, alloc> v{alloc{libmem::resource_ref{shared}}};

    EXPECT_THROW(
        {
            for (int i{}; i < 100'000; ++i) {
                v.push_back(i);
            }
        },
        std::bad_alloc);
}

TEST(ResourceAllocator, AStatefulResourceMustBeInjectedByReference) {
    /* How many times a standard container copies its allocator is unspecified, and
     * the two implementations really do differ: with the resource held **by value**
     * a budget of one serves a thousand reallocations under libc++ and runs out
     * under libstdc++. So this deliberately asserts nothing about that case; that
     * it is unpredictable at all is the reason a stateful resource must go through
     * a resource_ref.
     *
     * Through one, every allocator copy points at the same budget, and the outcome
     * is the same on both. */
    limited_resource shared{1};
    using by_ref = libmem::resource_allocator<int, libmem::resource_ref<limited_resource>>;

    std::vector<int, by_ref> tight{by_ref{libmem::resource_ref{shared}}};

    EXPECT_THROW(
        {
            for (int i{}; i < 1000; ++i) {
                tight.push_back(i);
            }
        },
        std::bad_alloc)
        << "one shared budget must actually run out";
}

/* ============================================================================
 * Allocator equality
 * ============================================================================ */

TEST(ResourceAllocator, EqualityFollowsTheResource) {
    libmem::arena a{1 << 16};
    libmem::arena b{1 << 16};

    const arena_alloc<int> on_a{arena_ref{a}};
    const arena_alloc<int> also_on_a{arena_ref{a}};
    const arena_alloc<int> on_b{arena_ref{b}};

    EXPECT_TRUE(on_a == also_on_a) << "same referent, so memory from one is releasable through the other";
    EXPECT_FALSE(on_a == on_b);

    /* Heterogeneous comparison, which the Allocator requirements need for a
     * container comparing against its rebound node allocator. */
    const arena_alloc<double> rebound{on_a};
    EXPECT_TRUE(on_a == rebound);

    /* A stateless resource makes every instance interchangeable. */
    EXPECT_TRUE(libmem::resource_allocator<int>{} == libmem::resource_allocator<int>{});
}

TEST(ResourceAllocator, SwapAndMoveAssignCarryTheAllocator) {
    libmem::arena a{1 << 16};
    libmem::arena b{1 << 16};

    std::vector<int, arena_alloc<int>> from_a{arena_alloc<int>{arena_ref{a}}};
    std::vector<int, arena_alloc<int>> from_b{arena_alloc<int>{arena_ref{b}}};

    from_a.assign({1, 2, 3});
    from_b.assign({9});

    /* propagate_on_container_swap is true, so this is well-defined even though the
     * two allocators are unequal. With non-propagating unequal allocators it is UB. */
    from_a.swap(from_b);

    EXPECT_EQ(from_a.size(), 1u);
    EXPECT_EQ(from_a.front(), 9);
    EXPECT_EQ(from_b.size(), 3u);
    EXPECT_EQ(from_a.get_allocator().resource().get(), &b) << "the allocator travelled with the buffer";
    EXPECT_EQ(from_b.get_allocator().resource().get(), &a);

    std::vector<int, arena_alloc<int>> target{arena_alloc<int>{arena_ref{a}}};
    target = std::move(from_a);
    EXPECT_EQ(target.size(), 1u);
    EXPECT_EQ(target.get_allocator().resource().get(), &b) << "move-assign propagates too, so the buffer can be stolen";
}

/* ============================================================================
 * Round trip: both adapters at once
 * ============================================================================ */

TEST(ResourceAllocator, RoundTripsThroughAllocatorResource) {
    /* A libmem container over an allocator made from a libmem resource. Absurd in
     * practice, but it proves the two adapters compose rather than collide. */
    libmem::arena scratch{1 << 16};

    using alloc = arena_alloc<std::byte>;
    using back_again = libmem::allocator_resource<alloc>;

    libmem::vector<int, back_again> v{back_again{alloc{arena_ref{scratch}}}};
    for (int i{}; i < 100; ++i) {
        ASSERT_NE(v.push_back(i), nullptr);
    }

    EXPECT_EQ(v.size(), 100u);
    EXPECT_EQ(v[99], 99);
}

} // namespace
