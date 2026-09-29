// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file multislab_test.cpp
 * @brief Tests for `libmem::multislab`: lazy growth, max-slab caps,
 *        full<->active list transitions, hysteresis-based shrinking, block
 *        iteration, and (via a counting resource) leak balance.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <ranges>
#include <vector>

import libmem;

using libmem::multislab;
using libmem::threshold_policy;

namespace {

constexpr std::size_t block = libmem::cache_line_size; // 64

/* A memory_resource that records allocation traffic through external counters
 * so balance can be checked after the multislab is destroyed. */
struct stats {
    std::size_t live_bytes{0};
    std::size_t allocs{0};
    std::size_t frees{0};
};

struct counting_resource {
    stats* s{};

    void* allocate(const std::size_t size) {
        ++s->allocs;
        s->live_bytes += size;
        return ::operator new(size);
    }

    void deallocate(void* ptr, const std::size_t size) noexcept {
        ++s->frees;
        s->live_bytes -= size;
        ::operator delete(ptr, size);
    }

    /* multislab takes its slab memory through these, so a resource backing one must be an
     * aligned_memory_resource. */
    void* allocate(const std::size_t size, const std::size_t align) {
        ++s->allocs;
        s->live_bytes += size;
        return ::operator new(size, std::align_val_t{align});
    }

    void deallocate(void* ptr, const std::size_t size, const std::size_t align) noexcept {
        ++s->frees;
        s->live_bytes -= size;
        ::operator delete(ptr, size, std::align_val_t{align});
    }
};

static_assert(libmem::memory_resource<counting_resource>);
static_assert(libmem::aligned_memory_resource<counting_resource>, "multislab needs the aligned overloads to align its slab memory");

TEST(MultislabTest, grows_lazily_on_first_allocation) {
    multislab<block, 4> ms{};
    EXPECT_EQ(ms.slab_count(), 0u); // nothing reserved until first use

    void* p{ms.allocate()};
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(ms.slab_count(), 1u);

    ms.deallocate(p);
}

TEST(MultislabTest, grows_across_multiple_slabs) {
    multislab<block, 2> ms{};

    std::array<void*, 5> blocks{}; // needs ceil(5/2) = 3 slabs
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }
    /* all distinct */
    for (std::size_t i{0}; i < blocks.size(); ++i) {
        for (std::size_t j{0}; j < i; ++j) {
            EXPECT_NE(blocks[i], blocks[j]);
        }
    }
    EXPECT_EQ(ms.slab_count(), 3u);

    for (auto* b : blocks) {
        ms.deallocate(b);
    }
}

TEST(MultislabTest, respects_max_slab_cap) {
    constexpr std::uint32_t max_slabs{2};
    multislab<block, 2> ms{max_slabs}; // capacity == 4 blocks

    std::array<void*, 4> blocks{};
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }

    /* capacity reached: further allocation fails without growing */
    EXPECT_EQ(ms.allocate(), nullptr);
    EXPECT_EQ(ms.slab_count(), max_slabs);

    for (auto* b : blocks) {
        ms.deallocate(b);
    }
}

/* A slab that fills up while it is still the active head must be returned to a
 * usable state on release without corrupting the active/full lists, and a
 * subsequent allocation must reuse existing capacity rather than growing. */
TEST(MultislabTest, full_to_active_transition_is_consistent) {
    multislab<block, 2> ms{};

    std::array<void*, 4> blocks{};
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }
    EXPECT_EQ(ms.slab_count(), 2u);

    /* free one slot, then re-allocate: capacity exists, so no new slab */
    ms.deallocate(blocks[0]);
    void* again{ms.allocate()};
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(ms.slab_count(), 2u);

    /* every still-live block must remain owned and releasable */
    ms.deallocate(again);
    ms.deallocate(blocks[1]);
    ms.deallocate(blocks[2]);
    ms.deallocate(blocks[3]);
}

TEST(MultislabTest, hysteresis_releases_empty_slabs) {
    /* reserve 0 empty slabs: an emptied slab is released, but never the last */
    multislab<block, 2> ms{threshold_policy{.max_empty_reserve = 0}};

    std::array<void*, 4> blocks{};
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }
    EXPECT_EQ(ms.slab_count(), 2u);

    for (auto* b : blocks) {
        ms.deallocate(b);
    }

    /* with zero reserve, all but the final slab are reclaimed */
    EXPECT_EQ(ms.slab_count(), 1u);
}

TEST(MultislabTest, iteration_counts_live_blocks) {
    multislab<block, 4> ms{};

    std::vector<void*> live{};
    for (int i{0}; i < 10; ++i) {
        void* p{ms.allocate()};
        ASSERT_NE(p, nullptr);
        live.push_back(p);
    }

    /* release a few, leaving 7 live */
    ms.deallocate(live[1]);
    ms.deallocate(live[4]);
    ms.deallocate(live[7]);

    const auto visited{std::ranges::distance(ms.begin(), ms.end())};
    EXPECT_EQ(visited, 7);

    /* clean up the rest */
    for (std::size_t i{0}; i < live.size(); ++i) {
        if (i != 1 && i != 4 && i != 7) {
            ms.deallocate(live[i]);
        }
    }
}

TEST(MultislabTest, iteration_covers_blocks_when_all_slabs_full) {
    /* Cap at one slab and fill it completely so the active list is empty and
     * every live block lives on the full list. Iteration must still find them. */
    constexpr std::uint32_t per_slab{4};
    multislab<block, per_slab> ms{1u}; // max 1 slab

    std::array<void*, per_slab> blocks{};
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }
    EXPECT_EQ(ms.allocate(), nullptr); // full, cannot grow

    const auto visited{std::ranges::distance(ms.begin(), ms.end())};
    EXPECT_EQ(visited, static_cast<std::ptrdiff_t>(per_slab));

    for (auto* b : blocks) {
        ms.deallocate(b);
    }
}

TEST(MultislabTest, backing_resource_is_balanced_after_destroy) {
    stats st{};
    {
        multislab<block, 3, counting_resource> ms{counting_resource{&st}};

        std::vector<void*> live{};
        for (int i{0}; i < 10; ++i) {
            void* p{ms.allocate()};
            ASSERT_NE(p, nullptr);
            live.push_back(p);
        }
        /* churn: free half, allocate more, then free everything */
        for (std::size_t i{0}; i < live.size(); i += 2) {
            ms.deallocate(live[i]);
        }
        for (std::size_t i{0}; i < live.size(); i += 2) {
            live[i] = ms.allocate();
            ASSERT_NE(live[i], nullptr);
        }
        for (auto* p : live) {
            ms.deallocate(p);
        }

        EXPECT_GT(st.allocs, 0u);
        ms.destroy();

        /* destroy must return every byte requested from the resource */
        EXPECT_EQ(st.allocs, st.frees);
        EXPECT_EQ(st.live_bytes, 0u);
    }
    /* destructor ran destroy_all again (idempotent); still balanced */
    EXPECT_EQ(st.allocs, st.frees);
    EXPECT_EQ(st.live_bytes, 0u);
}

} // namespace

/*
 * allocate_at() is the O(1) counterpart of allocate() + make_iterator(). It must
 * be observably identical: the iterator has to dereference to the block just
 * handed out, and walking on from it must visit exactly the same remaining
 * blocks as an iterator built the slow way.
 */
TEST(MultislabTest, allocate_at_matches_allocate_plus_make_iterator) {
    multislab<block, 4> ms{};

    std::vector<void*> live{};
    for (int i{0}; i < 10; ++i) {
        const auto alloc{ms.allocate_at()};
        ASSERT_NE(alloc.ptr, nullptr);

        /* Positioned at the block we were just given. */
        EXPECT_EQ(*alloc.it, alloc.ptr);

        /* And the onward traversal agrees with make_iterator(ptr). */
        std::vector<void*> from_allocate_at{};
        for (auto it{alloc.it}; it != ms.end(); ++it) {
            from_allocate_at.push_back(*it);
        }
        std::vector<void*> from_make_iterator{};
        for (auto it{ms.make_iterator(alloc.ptr)}; it != ms.end(); ++it) {
            from_make_iterator.push_back(*it);
        }
        EXPECT_EQ(from_allocate_at, from_make_iterator);

        live.push_back(alloc.ptr);
    }

    for (auto* p : live) {
        ms.deallocate(p);
    }
}

/*
 * allocate_at() must report failure the same way allocate() does, with an
 * end-equivalent iterator rather than one pointing into nothing.
 */
TEST(MultislabTest, allocate_at_yields_end_iterator_when_exhausted) {
    constexpr std::uint32_t per_slab{4};
    multislab<block, per_slab> ms{1u}; // max 1 slab

    std::array<void*, per_slab> blocks{};
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }

    const auto alloc{ms.allocate_at()};
    EXPECT_EQ(alloc.ptr, nullptr);
    EXPECT_EQ(alloc.it, ms.end());

    for (auto* b : blocks) {
        ms.deallocate(b);
    }
}

/*
 * Pin every block of a capped, completely full multislab so the owning node is
 * on the full list, and check make_iterator() still walks the remaining blocks.
 */
TEST(MultislabTest, make_iterator_handles_nodes_on_the_full_list) {
    constexpr std::uint32_t per_slab{4};
    multislab<block, per_slab> ms{1u}; // max 1 slab, so it ends up full

    std::array<void*, per_slab> blocks{};
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }
    ASSERT_EQ(ms.allocate(), nullptr);

    /* From the first block, iteration must still reach all four. */
    const auto visited{std::ranges::distance(ms.make_iterator(blocks[0]), ms.end())};
    EXPECT_EQ(visited, static_cast<std::ptrdiff_t>(per_slab));

    /* From the last block, exactly one. */
    EXPECT_EQ(std::ranges::distance(ms.make_iterator(blocks[per_slab - 1]), ms.end()), 1);

    for (auto* b : blocks) {
        ms.deallocate(b);
    }
}

/*
 * Erase-while-iterating across full slabs. Each release from a full slab moves
 * it back to the active list; the traversal must not follow that move.
 */
TEST(MultislabTest, releasing_during_iteration_visits_every_block_once) {
    constexpr std::uint32_t per_slab{4};
    multislab<block, per_slab> ms{};

    std::vector<void*> blocks(3 * per_slab);
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }

    std::vector<void*> visited{};
    for (auto it{ms.begin()}; it != ms.end();) {
        void* p{*it};
        ++it;
        visited.push_back(p);
        ms.deallocate(p);
    }

    std::ranges::sort(visited);
    std::ranges::sort(blocks);
    EXPECT_EQ(visited, blocks);
    EXPECT_EQ(ms.begin(), ms.end());
}

/*
 * Allocating during iteration fills a slab and moves it to the full list. Blocks
 * live before the traversal are visited exactly once; new ones at most once.
 */
TEST(MultislabTest, allocating_during_iteration_visits_existing_blocks_once) {
    constexpr std::uint32_t per_slab{4};
    multislab<block, per_slab> ms{};

    std::vector<void*> before(3 * per_slab - 1);
    for (auto& b : before) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }

    std::vector<void*> added{};
    std::vector<void*> visited{};
    for (auto it{ms.begin()}; it != ms.end(); ++it) {
        visited.push_back(*it);
        if (added.size() < 2) {
            added.push_back(ms.allocate());
        }
    }

    for (void* b : before) {
        EXPECT_EQ(std::ranges::count(visited, b), 1);
    }
    for (void* a : added) {
        EXPECT_LE(std::ranges::count(visited, a), 1);
    }
    EXPECT_LE(visited.size(), before.size() + added.size());

    for (void* p : before) {
        ms.deallocate(p);
    }
    for (void* p : added) {
        ms.deallocate(p);
    }
}

/*
 * A held iterator must survive another slab being emptied and released. Under
 * ASan this was a heap-use-after-free: the iterator kept a pointer to the head
 * of the full list it saw at construction.
 */
TEST(MultislabTest, held_iterator_survives_another_slab_being_freed) {
    constexpr std::uint32_t per_slab{4};
    multislab<block, per_slab> ms{threshold_policy{.max_empty_reserve = 0}};

    std::vector<void*> blocks(3 * per_slab);
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }

    /* blocks[4..7] fill the middle slab; release all of it while holding an
     * iterator elsewhere. */
    const std::vector<void*> released(blocks.begin() + per_slab, blocks.begin() + 2 * per_slab);
    auto it{ms.begin()};
    ASSERT_EQ(std::ranges::find(released, *it), released.end());

    for (void* b : released) {
        ms.deallocate(b);
    }
    ASSERT_EQ(ms.slab_count(), 2u);

    std::size_t walked{0};
    for (; it != ms.end(); ++it) {
        ++walked;
    }
    EXPECT_GE(walked, 1u);
    EXPECT_LE(walked, blocks.size() - per_slab);

    for (void* b : blocks) {
        if (std::ranges::find(released, b) == released.end()) {
            ms.deallocate(b);
        }
    }
}

/* A freed slot on a slab below the active head is reused before growing. */
TEST(MultislabTest, allocation_reuses_a_free_slot_before_growing) {
    constexpr std::uint32_t per_slab{4};

    for (const std::uint32_t cap : {0u, 2u}) {
        multislab<block, per_slab> ms{cap};

        std::vector<void*> blocks(per_slab + 1);
        for (auto& b : blocks) {
            b = ms.allocate();
            ASSERT_NE(b, nullptr);
        }
        ASSERT_EQ(ms.slab_count(), 2u);

        /* Free a slot in the full first slab, refill it, then allocate once more:
         * the second slab still has free slots. */
        ms.deallocate(blocks[0]);
        blocks[0] = ms.allocate();
        ASSERT_NE(blocks[0], nullptr);
        void* extra{ms.allocate()};
        ASSERT_NE(extra, nullptr) << "cap " << cap;
        EXPECT_EQ(ms.slab_count(), 2u) << "cap " << cap;

        ms.deallocate(extra);
        for (void* b : blocks) {
            ms.deallocate(b);
        }
    }
}

/*
 * deallocate(iterator) skips find_owner but must leave the same state as
 * deallocate(ptr): full->active moves, empty counts, and hysteresis frees.
 */
TEST(MultislabTest, deallocate_by_iterator_matches_deallocate_by_pointer) {
    constexpr std::uint32_t per_slab{4};
    const threshold_policy policy{.max_empty_reserve = 1};
    multislab<block, per_slab> by_ptr{policy};
    multislab<block, per_slab> by_it{policy};

    std::vector<void*> ptrs(3 * per_slab);
    std::vector<void*> its(3 * per_slab);
    for (std::size_t i{0}; i < ptrs.size(); ++i) {
        ptrs[i] = by_ptr.allocate();
        its[i] = by_it.allocate();
    }

    /* Empty the middle slab first, then the others, so both a kept and a
     * released empty slab are exercised. */
    for (const std::size_t i : {4u, 5u, 6u, 7u, 0u, 1u, 2u, 3u, 8u, 9u, 10u, 11u}) {
        by_ptr.deallocate(ptrs[i]);
        by_it.deallocate(by_it.make_iterator(its[i]));
        EXPECT_EQ(by_it.slab_count(), by_ptr.slab_count()) << "after " << i;
        EXPECT_EQ(by_it.empty_slab_count(), by_ptr.empty_slab_count()) << "after " << i;
        EXPECT_EQ(std::ranges::distance(by_it.begin(), by_it.end()), std::ranges::distance(by_ptr.begin(), by_ptr.end()));
    }
}

TEST(MultislabTest, deallocate_through_the_allocate_at_iterator) {
    multislab<block, 4> ms{};

    const auto kept{ms.allocate_at()};
    const auto dropped{ms.allocate_at()};
    ASSERT_NE(dropped.ptr, nullptr);

    ms.deallocate(dropped.it);
    EXPECT_EQ(std::ranges::distance(ms.begin(), ms.end()), 1);
    EXPECT_EQ(*ms.begin(), kept.ptr);
    ms.deallocate(kept.it);
    EXPECT_EQ(ms.empty_slab_count(), 1u);
}

namespace {

/* counting_resource whose slab-memory (aligned) allocation throws, after the node header was taken. */
struct slab_throwing_resource : counting_resource {
    using counting_resource::allocate;
    void* allocate(const std::size_t, const std::size_t) { throw std::bad_alloc{}; }
};

} // namespace

TEST(MultislabTest, grow_releases_the_node_when_the_slab_allocation_throws) {
    stats st{};
    {
        multislab<block, 4, slab_throwing_resource> ms{slab_throwing_resource{{&st}}};
        EXPECT_THROW(static_cast<void>(ms.allocate()), std::bad_alloc);
        EXPECT_EQ(ms.slab_count(), 0u);
    }
    EXPECT_EQ(st.live_bytes, 0u);
    EXPECT_EQ(st.allocs, st.frees);
}

/*
 * A double free, a foreign or misaligned pointer, and end() are rejected without
 * touching the bookkeeping. Before, a double free counted the slab down to empty
 * with a block still live and released it.
 */
TEST(MultislabTest, bad_deallocate_is_rejected) {
    multislab<block, 4> ms{threshold_policy{.max_empty_reserve = 0}};
    std::array<void*, 5> blocks{};
    for (auto& b : blocks) {
        b = ms.allocate();
        ASSERT_NE(b, nullptr);
    }
    std::int32_t foreign{};
    void* const misaligned{static_cast<std::byte*>(blocks[1]) + 1};

#ifdef NDEBUG
    ms.deallocate(blocks[0]);
    ms.deallocate(blocks[0]);
    ms.deallocate(&foreign);
    ms.deallocate(misaligned);
    ms.deallocate(decltype(ms.begin()){});
    ms.deallocate(blocks[1]);
    ms.deallocate(blocks[2]);

    /* blocks[3] and blocks[4] are still live, so no slab was released. */
    EXPECT_EQ(ms.slab_count(), 2u);
    EXPECT_EQ(std::ranges::distance(ms.begin(), ms.end()), 2);
    ms.deallocate(blocks[3]);
    ms.deallocate(blocks[4]);
#else
    ms.deallocate(blocks[0]);
    EXPECT_DEATH(ms.deallocate(blocks[0]), "double free");
    EXPECT_DEATH(ms.deallocate(&foreign), "not a block of this allocator");
    EXPECT_DEATH(ms.deallocate(misaligned), "not a block of this allocator");
    EXPECT_DEATH(ms.deallocate(decltype(ms.begin()){}), "end");
    for (std::size_t i{1}; i < blocks.size(); ++i) {
        ms.deallocate(blocks[i]);
    }
#endif
}
