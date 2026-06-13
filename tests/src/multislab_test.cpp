/**
 * @file multislab_test.cpp
 * @brief Tests for `libmem::multislab` — lazy growth, max-slab caps,
 *        full<->active list transitions, hysteresis-based shrinking, block
 *        iteration, and (via a counting resource) leak balance.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
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
};

static_assert(libmem::memory_resource<counting_resource>);

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

    /* capacity reached — further allocation fails without growing */
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
