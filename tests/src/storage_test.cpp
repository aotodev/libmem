#include <gtest/gtest.h>

import libmem;
import std;

namespace {

/* A payload that counts its own lifetime events, so a test can prove the storage
 * layer never constructs or destroys anything on its own. */
struct tracked {
    static int live;
    static int moves;

    int value{};

    explicit tracked(const int v) : value{v} { ++live; }
    tracked(const tracked& other) : value{other.value} { ++live; }
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
    }
};

int tracked::live{};
int tracked::moves{};

/* A payload whose move throws once a budget runs out, so the rollback path of
 * relocate_grow is actually instantiated and exercised. Every other payload in
 * the suite has a nothrow move, which leaves that branch of the `if constexpr`
 * discarded and therefore never even type-checked. */
struct throwing_move {
    static int live;
    static int move_budget;

    int value{};

    explicit throwing_move(const int v) : value{v} { ++live; }

    throwing_move(const throwing_move& other) : value{other.value} { ++live; }

    /* Deliberately not noexcept. */
    throwing_move(throwing_move&& other) : value{other.value} { // NOLINT(performance-noexcept-move-constructor)
        if (move_budget-- <= 0) {
            throw std::runtime_error{"throwing_move"};
        }
        ++live;
    }

    throwing_move& operator=(const throwing_move&) = default;
    throwing_move& operator=(throwing_move&&) = default;
    ~throwing_move() { --live; }

    static void reset() {
        live = 0;
        move_budget = 0;
    }
};

int throwing_move::live{};
int throwing_move::move_budget{};

static_assert(!std::is_nothrow_move_constructible_v<throwing_move>);

/**
 * @brief Resource that records the exact (size, alignment) of every live block and
 *        checks the pair it is given back.
 *
 * `-fsized-deallocation` is on and the resource interface is sized, so releasing a
 * block with a size that differs from the one it was allocated with is undefined
 * behaviour that neither ASan nor `arena`'s no-op `deallocate` would surface. This
 * catches it directly.
 */
class auditing_resource {
public:
    void* allocate(const std::size_t size) { return record(::operator new(size), size, 0); }

    void deallocate(void* ptr, const std::size_t size) noexcept {
        verify(ptr, size, 0);
        ::operator delete(ptr, size);
    }

    void* allocate(const std::size_t size, const std::size_t align) { return record(::operator new(size, std::align_val_t{align}), size, align); }

    void deallocate(void* ptr, const std::size_t size, const std::size_t align) noexcept {
        verify(ptr, size, align);
        ::operator delete(ptr, size, std::align_val_t{align});
    }

    /** @brief Blocks handed out and not yet returned. */
    std::size_t outstanding() const noexcept { return live_.size(); }

    /** @brief Times a block came back with a size or alignment it was not given. */
    int mismatches() const noexcept { return mismatches_; }

    std::size_t allocations() const noexcept { return allocations_; }

private:
    std::map<void*, std::pair<std::size_t, std::size_t>> live_{};
    int mismatches_{};
    std::size_t allocations_{};

    void* record(void* ptr, const std::size_t size, const std::size_t align) {
        live_[ptr] = {size, align};
        ++allocations_;
        return ptr;
    }

    void verify(void* ptr, const std::size_t size, const std::size_t align) noexcept {
        const auto found{live_.find(ptr)};
        if (found == live_.end() || found->second.first != size || found->second.second != align) {
            ++mismatches_;
            return;
        }
        live_.erase(found);
    }
};

static_assert(libmem::aligned_memory_resource<auditing_resource>);

/* A resource that fails after a set number of allocations, to exercise the
 * "storage could not supply the space" paths. */
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
 * Concept surface
 * ============================================================================ */

TEST(StorageConcepts, ExtentAndGrowthAreVisibleInTheType) {
    static_assert(libmem::storage_for<libmem::inline_storage<int, 4>, int>);
    static_assert(libmem::storage_for<libmem::fixed_storage<int, 4>, int>);
    static_assert(libmem::storage_for<libmem::dynamic_storage<int>, int>);

    static_assert(libmem::inline_storage<int, 4>::static_capacity == 4);
    static_assert(libmem::fixed_storage<int, 4>::static_capacity == 4);
    static_assert(libmem::dynamic_storage<int>::static_capacity == libmem::dynamic_extent);

    static_assert(!libmem::growable_storage<libmem::inline_storage<int, 4>>);
    static_assert(!libmem::growable_storage<libmem::fixed_storage<int, 4>>);
    static_assert(libmem::growable_storage<libmem::dynamic_storage<int>>);

    /* Only the heap-backed kinds transfer their slots on a move. */
    static_assert(!libmem::inline_storage<int, 4>::relocatable);
    static_assert(libmem::fixed_storage<int, 4>::relocatable);
    static_assert(libmem::dynamic_storage<int>::relocatable);
}

TEST(StorageConcepts, RebindKeepsTheResourceAndResetsTheAlignment) {
    using injected = libmem::dynamic_storage<int, libmem::resource_ref<libmem::arena>, libmem::cache_line_size>;
    using rebound = injected::rebind<char>;

    static_assert(std::same_as<rebound::value_type, char>);
    static_assert(std::same_as<rebound::resource_type, libmem::resource_ref<libmem::arena>>);
    static_assert(rebound::alignment == alignof(char));
    static_assert(injected::alignment == libmem::cache_line_size);
}

/* ============================================================================
 * inline_storage
 * ============================================================================ */

TEST(InlineStorage, HoldsItsSlotsInsideTheObject) {
    libmem::inline_storage<std::uint32_t, 16> store{};

    EXPECT_EQ(store.capacity(), 16u);
    EXPECT_GE(sizeof(store), 16u * sizeof(std::uint32_t));

    const auto* base{reinterpret_cast<const std::byte*>(&store)};
    const auto* slots{reinterpret_cast<const std::byte*>(store.data())};
    EXPECT_EQ(base, slots) << "slots should be the object's own bytes";
}

TEST(InlineStorage, HonoursAnExplicitAlignment) {
    libmem::inline_storage<std::uint32_t, 8, libmem::cache_line_size> store{};

    EXPECT_EQ(alignof(decltype(store)), libmem::cache_line_size);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(store.data()) % libmem::cache_line_size, 0u);
}

TEST(InlineStorage, ConstructsNothingOnItsOwn) {
    tracked::reset();
    {
        libmem::inline_storage<tracked, 8> store{};
        EXPECT_EQ(tracked::live, 0) << "storage is raw space, not a container";

        std::construct_at(store.data(), 42);
        EXPECT_EQ(tracked::live, 1);
        EXPECT_EQ(store.data()->value, 42);

        std::destroy_at(store.data());
    }
    EXPECT_EQ(tracked::live, 0);
}

/* ============================================================================
 * fixed_storage
 * ============================================================================ */

TEST(FixedStorage, KeepsTheExtentStaticButTheSlotsOffObject) {
    libmem::fixed_storage<std::uint64_t, 4096> store{};

    EXPECT_EQ(store.capacity(), 4096u);
    EXPECT_LT(sizeof(store), 4096u * sizeof(std::uint64_t)) << "slots must not be inline";

    /* Writable across the whole extent. */
    for (std::size_t i{}; i < store.capacity(); ++i) {
        std::construct_at(store.data() + i, i);
    }
    EXPECT_EQ(store.data()[4095], 4095u);
}

TEST(FixedStorage, HonoursAnExplicitAlignment) {
    libmem::fixed_storage<std::uint32_t, 8, libmem::default_resource, libmem::cache_line_size> store{};
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(store.data()) % libmem::cache_line_size, 0u);
}

TEST(FixedStorage, MoveTransfersTheSlots) {
    libmem::fixed_storage<int, 8> from{};
    int* slots{from.data()};

    libmem::fixed_storage<int, 8> to{std::move(from)};

    EXPECT_EQ(to.data(), slots);
    EXPECT_EQ(to.capacity(), 8u);

    /* The moved-from storage must report the capacity it actually has, not the
     * static extent: a container that trusted `N` here would walk slots it no
     * longer owns through a null pointer. */
    EXPECT_EQ(from.data(), nullptr);
    EXPECT_EQ(from.capacity(), 0u);
    /* The compile-time extent is unchanged; only the runtime capacity tracks it. */
    static_assert(libmem::fixed_storage<int, 8>::static_capacity == 8);
}

TEST(FixedStorage, TakesItsSlotsFromAnInjectedResource) {
    libmem::arena scratch{1 << 16};
    const std::size_t before{scratch.used()};

    libmem::fixed_storage<std::uint32_t, 64, libmem::resource_ref<libmem::arena>> store{libmem::resource_ref{scratch}};

    EXPECT_GE(scratch.used() - before, 64u * sizeof(std::uint32_t));
    EXPECT_NE(store.data(), nullptr);
}

/* ============================================================================
 * dynamic_storage
 * ============================================================================ */

TEST(DynamicStorage, StartsEmptyAndAllocatesNothing) {
    libmem::dynamic_storage<int> store{};

    EXPECT_EQ(store.capacity(), 0u);
    EXPECT_EQ(store.data(), nullptr);
}

TEST(DynamicStorage, ReserveBlockLeavesTheCurrentBlockLive) {
    libmem::dynamic_storage<int> store{};
    ASSERT_TRUE(libmem::relocate_grow(store, 4, 0));

    int* original{store.data()};
    std::construct_at(original, 7);

    const auto block{store.reserve_block(64)};
    ASSERT_TRUE(block);

    EXPECT_EQ(store.data(), original) << "reserve_block must not swap the block in";
    EXPECT_EQ(*original, 7);
    EXPECT_NE(block.data, original);

    store.discard(block);
    EXPECT_EQ(store.data(), original);
    EXPECT_EQ(*original, 7);
}

TEST(DynamicStorage, ReserveBlockReportsTheCapacityItActuallyAllocated) {
    libmem::dynamic_storage<std::uint64_t> store{};

    const auto block{store.reserve_block(1)};
    ASSERT_TRUE(block);

    /* Geometric growth happens inside reserve_block, and the sized deallocate on
     * the way out has to be given the real figure, so the block must report it. */
    EXPECT_GE(block.capacity, libmem::dynamic_storage<std::uint64_t>::min_capacity);
    store.adopt(block);
    EXPECT_EQ(store.capacity(), block.capacity);
}

TEST(DynamicStorage, GrowthIsGeometric) {
    libmem::dynamic_storage<std::uint64_t> store{};
    ASSERT_TRUE(libmem::relocate_grow(store, 1, 0));

    const std::size_t first{store.capacity()};
    ASSERT_TRUE(libmem::relocate_grow(store, first + 1, 0));

    EXPECT_GE(store.capacity(), 2 * first);
}

/* ============================================================================
 * relocate_grow
 * ============================================================================ */

TEST(RelocateGrow, MovesLiveElementsAndDestroysTheOriginals) {
    tracked::reset();
    {
        libmem::dynamic_storage<tracked> store{};
        ASSERT_TRUE(libmem::relocate_grow(store, 2, 0));

        std::construct_at(store.data() + 0, 10);
        std::construct_at(store.data() + 1, 20);
        ASSERT_EQ(tracked::live, 2);

        const std::size_t old_capacity{store.capacity()};
        ASSERT_TRUE(libmem::relocate_grow(store, old_capacity + 1, 2));

        EXPECT_EQ(tracked::live, 2) << "one live element per slot, no leaks and no double destroy";
        EXPECT_EQ(tracked::moves, 2);
        EXPECT_EQ(store.data()[0].value, 10);
        EXPECT_EQ(store.data()[1].value, 20);

        std::destroy_n(store.data(), 2);
    }
    EXPECT_EQ(tracked::live, 0);
}

TEST(RelocateGrow, IsANoOpWhenTheCapacityAlreadySuffices) {
    libmem::dynamic_storage<int> store{};
    ASSERT_TRUE(libmem::relocate_grow(store, 64, 0));

    int* slots{store.data()};
    EXPECT_TRUE(libmem::relocate_grow(store, 8, 0));
    EXPECT_EQ(store.data(), slots) << "no reallocation when it already fits";
}

TEST(RelocateGrow, LeavesTheStorageUntouchedWhenTheResourceIsExhausted) {
    libmem::dynamic_storage<int, limited_resource> store{limited_resource{1}};

    ASSERT_TRUE(libmem::relocate_grow(store, 4, 0));
    int* slots{store.data()};
    const std::size_t capacity{store.capacity()};
    std::construct_at(slots, 99);

    EXPECT_FALSE(libmem::relocate_grow(store, capacity + 1, 1)) << "budget spent";
    EXPECT_EQ(store.data(), slots) << "a failed growth must change nothing";
    EXPECT_EQ(store.capacity(), capacity);
    EXPECT_EQ(*slots, 99);

    std::destroy_at(slots);
}

TEST(RelocateGrow, RestoresTheStorageWhenAMoveThrowsMidRelocation) {
    throwing_move::reset();
    {
        libmem::dynamic_storage<throwing_move> store{};
        ASSERT_TRUE(libmem::relocate_grow(store, 4, 0));

        const std::size_t capacity{store.capacity()};
        ASSERT_GE(capacity, 4u) << "need room for the throw to land mid-relocation";

        for (std::size_t i{}; i < capacity; ++i) {
            std::construct_at(store.data() + i, static_cast<int>(i));
        }
        ASSERT_EQ(throwing_move::live, static_cast<int>(capacity));

        throwing_move* original{store.data()};
        throwing_move::move_budget = 2; // the third move throws

        EXPECT_THROW(static_cast<void>(libmem::relocate_grow(store, capacity + 1, capacity)), std::runtime_error);

        /* The whole point of reserve/adopt/discard being three steps: the old block
         * is still the live one and still holds every element. */
        EXPECT_EQ(store.data(), original) << "a throwing move must not swap the block in";
        EXPECT_EQ(store.capacity(), capacity);
        EXPECT_EQ(throwing_move::live, static_cast<int>(capacity)) << "the partially built new block must be cleaned up, and no original destroyed";

        for (std::size_t i{}; i < capacity; ++i) {
            EXPECT_EQ(store.data()[i].value, static_cast<int>(i)) << "element " << i;
        }

        std::destroy_n(store.data(), capacity);
    }
    EXPECT_EQ(throwing_move::live, 0);
}

TEST(RelocateGrow, SucceedsWithAThrowingMoveWhenTheBudgetHolds) {
    throwing_move::reset();
    {
        libmem::dynamic_storage<throwing_move> store{};
        ASSERT_TRUE(libmem::relocate_grow(store, 4, 0));

        const std::size_t capacity{store.capacity()};
        std::construct_at(store.data() + 0, 10);
        std::construct_at(store.data() + 1, 20);

        throwing_move::move_budget = 1000;
        ASSERT_TRUE(libmem::relocate_grow(store, capacity + 1, 2));

        EXPECT_EQ(throwing_move::live, 2);
        EXPECT_EQ(store.data()[0].value, 10);
        EXPECT_EQ(store.data()[1].value, 20);

        std::destroy_n(store.data(), 2);
    }
    EXPECT_EQ(throwing_move::live, 0);
}

/* ============================================================================
 * Sized deallocation
 * ============================================================================ */

TEST(SizedDeallocation, EveryBlockGoesBackWithTheSizeItCameWith) {
    auditing_resource audit{};
    {
        libmem::dynamic_storage<std::uint64_t, libmem::resource_ref<auditing_resource>> store{libmem::resource_ref{audit}};

        /* Several growths, so reserve_block's geometric rounding is what release
         * has to hand back, not the requested figure. */
        for (std::size_t n{1}; n <= 5000; n *= 4) {
            ASSERT_TRUE(libmem::relocate_grow(store, n, 0));
        }
        EXPECT_GT(audit.allocations(), 1u) << "the test needs to have actually grown";
        EXPECT_EQ(audit.mismatches(), 0);
    }
    EXPECT_EQ(audit.outstanding(), 0u) << "every block released";
    EXPECT_EQ(audit.mismatches(), 0) << "a size or alignment mismatch reaching deallocate is UB";
}

TEST(SizedDeallocation, HoldsForAnOverAlignedFixedStorageToo) {
    auditing_resource audit{};
    {
        libmem::fixed_storage<std::uint32_t, 256, libmem::resource_ref<auditing_resource>, libmem::cache_line_size> store{libmem::resource_ref{audit}};
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(store.data()) % libmem::cache_line_size, 0u);
        EXPECT_EQ(audit.outstanding(), 1u);
    }
    EXPECT_EQ(audit.outstanding(), 0u);
    EXPECT_EQ(audit.mismatches(), 0) << "the aligned allocate and the aligned deallocate must pair up";
}

/* ============================================================================
 * Resource adapters
 * ============================================================================ */

TEST(ResourceRef, LetsSeveralContainersShareOneArena) {
    libmem::arena scratch{1 << 16};

    libmem::dynamic_storage<int, libmem::resource_ref<libmem::arena>> a{libmem::resource_ref{scratch}};
    libmem::dynamic_storage<int, libmem::resource_ref<libmem::arena>> b{libmem::resource_ref{scratch}};

    ASSERT_TRUE(libmem::relocate_grow(a, 16, 0));
    ASSERT_TRUE(libmem::relocate_grow(b, 16, 0));

    EXPECT_NE(a.data(), b.data());
    EXPECT_EQ(a.resource().get(), &scratch);
    EXPECT_EQ(b.resource().get(), &scratch);
}

TEST(ResourceRef, ForwardsTheAlignedOverloads) {
    /* Wrapping a resource must not silently downgrade it to default_alignment:
     * that is the failure mode the aligned refinement exists to catch. */
    static_assert(libmem::aligned_memory_resource<libmem::arena>);
    static_assert(libmem::aligned_memory_resource<libmem::resource_ref<libmem::arena>>);

    libmem::arena scratch{1 << 16};
    libmem::fixed_storage<std::uint32_t, 32, libmem::resource_ref<libmem::arena>, libmem::cache_line_size> store{libmem::resource_ref{scratch}};

    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(store.data()) % libmem::cache_line_size, 0u);
}

TEST(AllocatorResource, AdaptsAStandardAllocator) {
    static_assert(libmem::memory_resource<libmem::allocator_resource<std::allocator<int>>>);
    /* Deliberately not aligned-capable: the Allocator interface cannot express a
     * runtime alignment, so over-aligned storage refuses to instantiate rather
     * than quietly under-aligning. */
    static_assert(!libmem::aligned_memory_resource<libmem::allocator_resource<std::allocator<int>>>);

    libmem::dynamic_storage<int, libmem::allocator_resource<std::allocator<int>>> store{};
    ASSERT_TRUE(libmem::relocate_grow(store, 32, 0));

    EXPECT_GE(store.capacity(), 32u);
    std::construct_at(store.data(), 5);
    EXPECT_EQ(*store.data(), 5);
    std::destroy_at(store.data());
}

} // namespace
