#include <gtest/gtest.h>

import libmem;
import std;

namespace {

struct command {
    std::uint32_t kind{};
    std::uint32_t value{};
};

using ring = libmem::spsc_ring<command, 8>;

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

} // namespace
