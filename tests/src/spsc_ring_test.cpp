#include <gtest/gtest.h>

import libmem;
import std;

namespace {

struct command {
    std::uint32_t kind{};
    std::uint32_t value{};
};

using ring = libmem::spsc_ring<command, 8>;

/* Constant initialisation, which is as far as constant evaluation reaches for a
 * ring: std::atomic has a constexpr constructor but no constexpr load or store, so
 * one can be built at compile time and not used there. Worth having because a ring
 * is usually a global: this has no dynamic initialiser and so cannot take part in
 * the static-initialisation-order fiasco. */
constinit libmem::spsc_ring<std::uint32_t, 4> static_queue{};

TEST(SpscRing, IsConstantInitialised) {
    EXPECT_TRUE(static_queue.empty());
    EXPECT_EQ(static_queue.size_approx(), 0u);

    ASSERT_TRUE(static_queue.try_push(7));
    std::uint32_t out{};
    ASSERT_TRUE(static_queue.try_pop(out));
    EXPECT_EQ(out, 7u);
}

TEST(SpscRing, StartsEmpty) {
    ring q{};

    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.full());
    EXPECT_EQ(q.size_approx(), 0u);

    command out{};
    EXPECT_FALSE(q.try_pop(out));
}

TEST(SpscRing, CapacityReservesOneSlot) {
    EXPECT_EQ(ring::capacity(), 8u);
    EXPECT_EQ(ring::max_size(), 7u);
}

TEST(SpscRing, FillsToMaxSizeThenRefuses) {
    ring q{};

    for (std::uint32_t i{}; i < ring::max_size(); ++i) {
        EXPECT_TRUE(q.try_push({.kind = 1, .value = i})) << "push " << i;
    }

    EXPECT_TRUE(q.full());
    EXPECT_EQ(q.size_approx(), ring::max_size());
    EXPECT_FALSE(q.try_push({.kind = 1, .value = 99}));
}

TEST(SpscRing, PreservesOrder) {
    ring q{};

    for (std::uint32_t i{}; i < ring::max_size(); ++i) {
        ASSERT_TRUE(q.try_push({.kind = 7, .value = i}));
    }

    for (std::uint32_t i{}; i < ring::max_size(); ++i) {
        command out{};
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out.kind, 7u);
        EXPECT_EQ(out.value, i);
    }

    EXPECT_TRUE(q.empty());
}

TEST(SpscRing, WrapsAroundManyTimes) {
    ring q{};

    /* Several laps of the buffer, one in flight at a time, so every slot is reused. */
    for (std::uint32_t i{}; i < ring::capacity() * 10; ++i) {
        ASSERT_TRUE(q.try_push({.kind = 2, .value = i}));

        command out{};
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out.value, i);
    }

    EXPECT_TRUE(q.empty());
}

TEST(SpscRing, FailedPopLeavesOutputUntouched) {
    ring q{};

    command out{.kind = 42, .value = 43};
    EXPECT_FALSE(q.try_pop(out));
    EXPECT_EQ(out.kind, 42u);
    EXPECT_EQ(out.value, 43u);
}

TEST(SpscRing, ConsumeAllDrainsAndFrees) {
    ring q{};

    for (std::uint32_t i{}; i < 5; ++i) {
        ASSERT_TRUE(q.try_push({.kind = 3, .value = i}));
    }

    std::vector<std::uint32_t> seen{};
    const auto consumed{q.consume_all([&seen](const command& c) { seen.push_back(c.value); })};

    EXPECT_EQ(consumed, 5u);
    EXPECT_EQ(seen, (std::vector<std::uint32_t>{0, 1, 2, 3, 4}));
    EXPECT_TRUE(q.empty());

    /* The slots are free again, so the queue takes a full batch afterwards. */
    for (std::uint32_t i{}; i < ring::max_size(); ++i) {
        EXPECT_TRUE(q.try_push({.kind = 4, .value = i}));
    }
}

TEST(SpscRing, ConsumeAllOnEmptyDoesNothing) {
    ring q{};

    std::size_t calls{};
    EXPECT_EQ(q.consume_all([&calls](const command&) { ++calls; }), 0u);
    EXPECT_EQ(calls, 0u);
}

TEST(SpscRing, IndicesSitOnSeparateCacheLines) {
    /* False sharing between the producer's and consumer's lines is the whole reason for
     * the alignment, so it is worth asserting rather than trusting. The two atomics are
     * private, so this measures the object instead: its size is a whole number of lines
     * and its address is line-aligned. */
    ring q{};

    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&q) % libmem::cache_line_size, 0u);
    EXPECT_EQ(sizeof(ring) % libmem::cache_line_size, 0u);

    /* Slots, then two index lines: never smaller than three lines. */
    EXPECT_GE(sizeof(ring), libmem::cache_line_size * 3);
}

TEST(SpscRing, SurvivesTwoThreadHandoff) {
    /* The point of the type: run it for real under whatever the sanitiser build enables.
     * TSan flags a missing edge here; ASan flags an out-of-range slot index. */
    constexpr std::uint32_t total{200'000};

    libmem::spsc_ring<std::uint32_t, 64> q{};
    std::uint64_t sum{};

    std::thread consumer{[&q, &sum] {
        std::uint32_t received{};
        std::uint32_t value{};

        while (received < total) {
            if (q.try_pop(value)) {
                sum += value;
                ++received;
            } else {
                std::this_thread::yield();
            }
        }
    }};

    for (std::uint32_t i{}; i < total; ++i) {
        while (!q.try_push(i)) {
            std::this_thread::yield();
        }
    }

    consumer.join();

    constexpr std::uint64_t expected{static_cast<std::uint64_t>(total - 1) * total / 2};
    EXPECT_EQ(sum, expected);
    EXPECT_TRUE(q.empty());
}

TEST(SpscRing, BatchDrainSeesEveryElementInOrder) {
    constexpr std::uint32_t total{100'000};

    libmem::spsc_ring<std::uint32_t, 32> q{};
    std::atomic<bool> ordered{true};

    std::thread consumer{[&q, &ordered] {
        std::uint32_t received{};
        std::uint32_t next{};

        while (received < total) {
            received += static_cast<std::uint32_t>(q.consume_all([&next, &ordered](const std::uint32_t v) {
                if (v != next) {
                    ordered.store(false, std::memory_order_relaxed);
                }
                ++next;
            }));

            if (received < total) {
                std::this_thread::yield();
            }
        }
    }};

    for (std::uint32_t i{}; i < total; ++i) {
        while (!q.try_push(i)) {
            std::this_thread::yield();
        }
    }

    consumer.join();

    EXPECT_TRUE(ordered.load(std::memory_order_relaxed));
    EXPECT_TRUE(q.empty());
}

/* ============================================================================
 * Storage variants: same ring, different home for the slots
 * ============================================================================ */

TEST(SpscRingStorage, InlineAndHeapVariantsAreTheSameRing) {
    using inline_ring = libmem::spsc_ring<command, 64>;
    using heap_ring = libmem::heap_spsc_ring<command, 64>;

    static_assert(inline_ring::capacity() == heap_ring::capacity());
    static_assert(inline_ring::max_size() == heap_ring::max_size());
    static_assert(std::same_as<inline_ring, libmem::basic_spsc_ring<libmem::inline_storage<command, 64, libmem::cache_line_size>>>);
}

TEST(SpscRingStorage, HeapVariantKeepsTheObjectSmall) {
    /* The point of the heap variant: a capacity too large to sit inline, with the
     * mask still a compile-time constant. */
    using big = libmem::heap_spsc_ring<command, 1 << 16>;

    EXPECT_LT(sizeof(big), 4u * libmem::cache_line_size);
    EXPECT_EQ(sizeof(big), sizeof(libmem::heap_spsc_ring<command, 4>));
    EXPECT_GT(sizeof(libmem::spsc_ring<command, 1 << 16>), (1u << 16) * sizeof(command));
}

TEST(SpscRingStorage, HeapVariantRoundTripsElements) {
    libmem::heap_spsc_ring<command, 1 << 14> q{};

    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.capacity(), 1u << 14);

    for (std::uint32_t i{}; i < 1000; ++i) {
        ASSERT_TRUE(q.try_push({.kind = 1, .value = i})) << "push " << i;
    }

    std::uint32_t next{};
    const auto consumed{q.consume_all([&next](const command& c) {
        EXPECT_EQ(c.value, next);
        ++next;
    })};

    EXPECT_EQ(consumed, 1000u);
    EXPECT_TRUE(q.empty());
}

TEST(SpscRingStorage, HeapVariantFillsToMaxSizeThenRefuses) {
    libmem::heap_spsc_ring<command, 8> q{};

    for (std::uint32_t i{}; i < q.max_size(); ++i) {
        ASSERT_TRUE(q.try_push({.kind = 1, .value = i}));
    }

    EXPECT_TRUE(q.full());
    EXPECT_FALSE(q.try_push({.kind = 1, .value = 99}));

    command out{};
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.value, 0u);
    EXPECT_TRUE(q.try_push({.kind = 1, .value = 99})) << "a slot freed up";
}

TEST(SpscRingStorage, HeapVariantTakesItsSlotsFromAnInjectedArena) {
    libmem::arena scratch{1 << 20};
    const std::size_t before{scratch.used()};

    libmem::heap_spsc_ring<command, 1024, libmem::resource_ref<libmem::arena>> q{libmem::resource_ref{scratch}};

    EXPECT_GE(scratch.used() - before, 1024u * sizeof(command));

    ASSERT_TRUE(q.try_push({.kind = 3, .value = 7}));
    command out{};
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.kind, 3u);
    EXPECT_EQ(out.value, 7u);
}

TEST(SpscRingStorage, HeapVariantSlotsAreCacheLineAligned) {
    libmem::heap_spsc_ring<command, 64> q{};

    /* The ring itself is cache-line aligned in both variants, so a neighbouring
     * object cannot land in the consumer's line. */
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&q) % libmem::cache_line_size, 0u);
    EXPECT_EQ(sizeof(q) % libmem::cache_line_size, 0u);
}

TEST(SpscRingStorage, HeapVariantSurvivesTheThreadedHandoff) {
    constexpr std::uint32_t total{200000};
    libmem::heap_spsc_ring<std::uint32_t, 1024> q{};

    std::atomic<bool> ordered{true};

    std::thread consumer{[&q, &ordered] {
        std::uint32_t received{};
        std::uint32_t next{};

        while (received < total) {
            received += static_cast<std::uint32_t>(q.consume_all([&next, &ordered](const std::uint32_t v) {
                if (v != next) {
                    ordered.store(false, std::memory_order_relaxed);
                }
                ++next;
            }));

            if (received < total) {
                std::this_thread::yield();
            }
        }
    }};

    for (std::uint32_t i{}; i < total; ++i) {
        while (!q.try_push(i)) {
            std::this_thread::yield();
        }
    }

    consumer.join();

    EXPECT_TRUE(ordered.load(std::memory_order_relaxed));
    EXPECT_TRUE(q.empty());
}

} // namespace
