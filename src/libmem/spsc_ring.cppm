/**
 * @file spsc_ring.cppm
 * @brief Lock-free single-producer single-consumer ring buffer with cached indices.
 *
 * @code
 *     libmem::spsc_ring<command, 64> queue{};                      // slots inline
 *     libmem::heap_spsc_ring<command, 1 << 20> big{};              // same ring, slots on the heap
 *
 *     if (!queue.try_push(cmd)) { ... }                            // producer thread
 *     queue.consume_all([](const command& c) { apply(c); });       // consumer thread
 * @endcode
 *
 * The capacity is a template parameter for every variant, so the index mask read
 * on each push and pop stays a compile-time constant. The storage argument
 * chooses only *where* the slots live: `spsc_ring<T, N>` puts them in the object,
 * `heap_spsc_ring<T, N>` takes them from a `memory_resource` so a large `N` need
 * not fit on the stack or inside an enclosing struct.
 *
 * @warning Exactly one thread may call the producer half (`try_push`, `full`) and exactly
 *          one other the consumer half (`try_pop`, `consume_all`, `empty`). Two of either
 *          breaks it. Construct and destroy with neither side running.
 */
export module libmem:spsc_ring;

import :concepts;
import :storage;
import std;

namespace libmem {

/**
 * @brief Lock-free SPSC ring buffer over a fixed-extent `storage`.
 *
 * Each side keeps a plain snapshot of the other's index and refreshes it, with an acquire
 * load, only when it claims the queue is full or empty. A snapshot can only be stale in the
 * safe direction, so the shared cache lines stay quiet in steady state.
 *
 * @tparam Storage  Fixed-extent storage supplying the slots. Its capacity is the
 *                  ring's: a power of two, at least 2. Its `value_type` must be
 *                  trivially copyable, so a transfer runs no destructor on the
 *                  consumer thread, and default-initialisable, since every slot
 *                  is value-constructed up front.
 *
 * Prefer the `spsc_ring` / `heap_spsc_ring` aliases below over naming this directly.
 */
export template <typename Storage>
    requires fixed_extent_storage<Storage> && (Storage::static_capacity >= 2) && ((Storage::static_capacity & (Storage::static_capacity - 1)) == 0) &&
             std::is_trivially_copyable_v<typename Storage::value_type> && std::default_initializable<typename Storage::value_type>
class alignas(cache_line_size) basic_spsc_ring {
public:
    using storage_type = Storage;
    using value_type = typename Storage::value_type;
    using size_type = std::size_t;

    static_assert(std::atomic<size_type>::is_always_lock_free, "spsc_ring needs lock-free index atomics");

    /** @brief Construct with a default-constructed storage. */
    constexpr basic_spsc_ring() noexcept(std::is_nothrow_default_constructible_v<value_type>)
        requires std::default_initializable<Storage>
    {
        construct_slots();
    }

    /**
     * @brief Construct the storage from `args`, then the slots.
     *
     * This is how a resource reaches a heap-backed ring:
     * `heap_spsc_ring<T, N, resource_ref<arena>> ring{resource_ref{scratch}}`.
     */
    template <typename... Args>
        requires(sizeof...(Args) > 0) && std::constructible_from<Storage, Args...>
    constexpr explicit basic_spsc_ring(Args&&... args) noexcept(std::is_nothrow_default_constructible_v<value_type>) : storage_{std::forward<Args>(args)...} {
        construct_slots();
    }

    basic_spsc_ring(const basic_spsc_ring&) = delete;
    basic_spsc_ring& operator=(const basic_spsc_ring&) = delete;
    basic_spsc_ring(basic_spsc_ring&&) = delete;
    basic_spsc_ring& operator=(basic_spsc_ring&&) = delete;

    constexpr ~basic_spsc_ring() { std::destroy_n(storage_.data(), capacity()); }

    /// @brief Allocated slots.
    static constexpr size_type capacity() noexcept { return Storage::static_capacity; }

    /// @brief Elements held at once. One slot is reserved, so that `head == tail` can mean empty.
    static constexpr size_type max_size() noexcept { return capacity() - 1; }

    /* --- producer ----------------------------------------------------- */

    /**
     * @brief Copy one element in. Producer thread only.
     * @return `false` when full, in which case nothing was written.
     */
    bool try_push(const value_type& value) noexcept {
        /* Relaxed: the producer is the only writer of head. */
        const auto head{head_.load(std::memory_order_relaxed)};
        const auto next{(head + 1) & mask};

        if (next == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);

            if (next == cached_tail_) {
                return false;
            }
        }

        storage_.data()[head] = value;

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
    bool try_pop(value_type& out) noexcept {
        const auto tail{tail_.load(std::memory_order_relaxed)};

        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);

            if (tail == cached_head_) {
                return false;
            }
        }

        out = storage_.data()[tail];

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
        requires std::invocable<F&, const value_type&>
    size_type consume_all(F&& fn) noexcept(std::is_nothrow_invocable_v<F&, const value_type&>) {
        auto tail{tail_.load(std::memory_order_relaxed)};
        cached_head_ = head_.load(std::memory_order_acquire);

        const value_type* slots{storage_.data()};
        size_type consumed{};

        while (tail != cached_head_) {
            fn(slots[tail]);
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
    static constexpr size_type mask{Storage::static_capacity - 1};

    constexpr void construct_slots() noexcept(std::is_nothrow_default_constructible_v<value_type>) {
        /* Every slot is a live object for the whole life of the ring: try_push
         * assigns into one, which needs an object there to assign to. */
        detail::value_construct_n(storage_.data(), capacity());
    }

    /* The slots come first so an inline storage, whose alignment the aliases below raise
     * to a cache line, starts one. For a heap-backed storage this member is just the
     * resource and a pointer: written once at construction, read by both sides, and the
     * `alignas` on head_ keeps it off the producer's line. */
    Storage storage_{};

    /* One cache line per side, each holding the index that side writes and its snapshot of
     * the other, so refreshing a snapshot never dirties the other side's line. */
    alignas(cache_line_size) std::atomic<size_type> head_{};
    size_type cached_tail_{};

    alignas(cache_line_size) std::atomic<size_type> tail_{};
    size_type cached_head_{};
};

/**
 * @brief SPSC ring holding its `Capacity` slots inline, allocating nothing.
 *
 * The slot array is cache-line aligned, which costs nothing here and keeps the
 * first slot out of a neighbour's line.
 */
export template <typename T, std::size_t Capacity> using spsc_ring = basic_spsc_ring<inline_storage<T, Capacity, cache_line_size>>;

/**
 * @brief SPSC ring taking its `Capacity` slots from a `memory_resource`.
 *
 * `sizeof` is three cache lines regardless of `Capacity`. The mask is still a
 * compile-time constant.
 *
 * @note The single allocation happens at construction and is asserted to succeed:
 *       a ring that could not get its slots has no usable degraded state.
 */
export template <typename T, std::size_t Capacity, memory_resource Resource = default_resource>
using heap_spsc_ring = basic_spsc_ring<fixed_storage<T, Capacity, Resource, cache_line_size>>;

/* The class alignment rounds `sizeof` up to whole cache lines, which also keeps a
 * neighbouring object out of the consumer's line. */
static_assert(sizeof(spsc_ring<std::uint32_t, 4>) % cache_line_size == 0);
static_assert(alignof(spsc_ring<std::uint32_t, 4>) == cache_line_size);

static_assert(sizeof(heap_spsc_ring<std::uint32_t, 4>) % cache_line_size == 0);
static_assert(alignof(heap_spsc_ring<std::uint32_t, 4>) == cache_line_size);

/* The heap variant's footprint is the two index lines plus the resource, not the slots. */
static_assert(sizeof(heap_spsc_ring<std::uint32_t, 1 << 20>) == sizeof(heap_spsc_ring<std::uint32_t, 4>));

} // namespace libmem
