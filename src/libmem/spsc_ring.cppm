/**
 * @file spsc_ring.cppm
 * @brief Lock-free single-producer single-consumer ring buffer with cached indices.
 *
 * @code
 *     libmem::spsc_ring<command, 64> queue{};
 *
 *     if (!queue.try_push(cmd)) { ... }                            // producer thread
 *     queue.consume_all([](const command& c) { apply(c); });       // consumer thread
 * @endcode
 *
 * @warning Exactly one thread may call the producer half (`try_push`, `full`) and exactly
 *          one other the consumer half (`try_pop`, `consume_all`, `empty`). Two of either
 *          breaks it. Construct and destroy with neither side running.
 */
export module libmem:spsc_ring;

import :concepts;
import std;

namespace libmem {

/**
 * @brief Lock-free SPSC ring buffer over inline storage.
 *
 * Each side keeps a plain snapshot of the other's index and refreshes it, with an acquire
 * load, only when it claims the queue is full or empty. A snapshot can only be stale in the
 * safe direction, so the shared cache lines stay quiet in steady state.
 *
 * @tparam T         Element type. Trivially copyable, so a transfer runs no destructor on
 *                   the consumer thread.
 * @tparam Capacity  Slot count, a power of two. One slot is reserved, see `max_size()`.
 */
export template <typename T, std::size_t Capacity>
    requires(Capacity >= 2) && ((Capacity & (Capacity - 1)) == 0) && std::is_trivially_copyable_v<T> && std::default_initializable<T>
class alignas(cache_line_size) spsc_ring {
public:
    using value_type = T;
    using size_type = std::size_t;

    static_assert(std::atomic<size_type>::is_always_lock_free, "spsc_ring needs lock-free index atomics");

    constexpr spsc_ring() noexcept = default;

    spsc_ring(const spsc_ring&) = delete;
    spsc_ring& operator=(const spsc_ring&) = delete;
    spsc_ring(spsc_ring&&) = delete;
    spsc_ring& operator=(spsc_ring&&) = delete;

    /// @brief Allocated slots.
    static constexpr size_type capacity() noexcept { return Capacity; }

    /// @brief Elements held at once. One slot is reserved, so that `head == tail` can mean empty.
    static constexpr size_type max_size() noexcept { return Capacity - 1; }

    /* --- producer ----------------------------------------------------- */

    /**
     * @brief Copy one element in. Producer thread only.
     * @return `false` when full, in which case nothing was written.
     */
    bool try_push(const T& value) noexcept {
        /* Relaxed: the producer is the only writer of head. */
        const auto head{head_.load(std::memory_order_relaxed)};
        const auto next{(head + 1) & mask};

        if (next == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);

            if (next == cached_tail_) {
                return false;
            }
        }

        slots_[head] = value;

        /* Release: publishes the slot write to a consumer that observes this head. */
        head_.store(next, std::memory_order_release);

        return true;
    }

    /// @brief Whether `try_push` would fail now. Producer thread only; may be pessimistic.
    bool full() const noexcept {
        const auto next{(head_.load(std::memory_order_relaxed) + 1) & mask};
        return next == tail_.load(std::memory_order_acquire);
    }

    /* --- consumer ----------------------------------------------------- */

    /**
     * @brief Copy one element out. Consumer thread only.
     * @return `false` when empty, in which case `out` is untouched.
     */
    bool try_pop(T& out) noexcept {
        const auto tail{tail_.load(std::memory_order_relaxed)};

        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);

            if (tail == cached_head_) {
                return false;
            }
        }

        out = slots_[tail];

        /* Release: keeps the producer from overwriting the slot before the copy is done. */
        tail_.store((tail + 1) & mask, std::memory_order_release);

        return true;
    }

    /**
     * @brief Apply `fn` to every queued element, then free them all at once.
     *
     * One acquire load and one release store for the batch rather than per element. `fn`
     * runs before the slots are released, so anything it keeps must be copied; it must not
     * push to this queue and must not throw.
     *
     * @return How many elements were consumed.
     */
    template <typename F>
        requires std::invocable<F&, const T&>
    size_type consume_all(F&& fn) noexcept(std::is_nothrow_invocable_v<F&, const T&>) {
        auto tail{tail_.load(std::memory_order_relaxed)};
        cached_head_ = head_.load(std::memory_order_acquire);

        size_type consumed{};

        while (tail != cached_head_) {
            fn(static_cast<const T&>(slots_[tail]));
            tail = (tail + 1) & mask;
            ++consumed;
        }

        if (consumed > 0) {
            tail_.store(tail, std::memory_order_release);
        }

        return consumed;
    }

    /// @brief Whether `try_pop` would fail now. Consumer thread only; may be pessimistic.
    bool empty() const noexcept { return tail_.load(std::memory_order_relaxed) == head_.load(std::memory_order_acquire); }

    /// @brief Queued count. Either side may call it, but it is only ever a snapshot: metrics, not control flow.
    size_type size_approx() const noexcept {
        const auto head{head_.load(std::memory_order_acquire)};
        const auto tail{tail_.load(std::memory_order_acquire)};
        return (head - tail) & mask;
    }

private:
    static constexpr size_type mask{Capacity - 1};

    alignas(cache_line_size) std::array<T, Capacity> slots_{};

    /* One cache line per side, each holding the index that side writes and its snapshot of
     * the other, so refreshing a snapshot never dirties the other side's line. */
    alignas(cache_line_size) std::atomic<size_type> head_{};
    size_type cached_tail_{};

    alignas(cache_line_size) std::atomic<size_type> tail_{};
    size_type cached_head_{};
};

/* The class alignment rounds `sizeof` up to whole cache lines, which also keeps a
 * neighbouring object out of the consumer's line. */
static_assert(sizeof(spsc_ring<std::uint32_t, 4>) % cache_line_size == 0);
static_assert(alignof(spsc_ring<std::uint32_t, 4>) == cache_line_size);

} // namespace libmem
