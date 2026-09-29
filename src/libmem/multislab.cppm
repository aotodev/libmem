// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file multislab.cppm
 * @brief Auto-expanding multi-slab allocator with hysteresis-based shrinking.
 *
 * Chains multiple `slab` instances together so that allocation never fails as
 * long as the backing memory resource can supply more pages. Empty slabs are
 * released according to a configurable hysteresis policy, keeping a small
 * reserve to avoid repeated grow/shrink cycles.
 *
 * The backing memory resource is injected as a concept-constrained template
 * parameter, so there is no virtual dispatch. Iteration over all allocated
 * blocks is exposed as a `std::ranges::input_range`.
 *
 * @code
 *     libmem::multislab<4096, 64> pool{};
 *     void* blk = pool.allocate();
 *     pool.deallocate(blk);
 *     for (void* p : pool) { ... }
 * @endcode
 */
module;

#include <cassert>

export module libmem:multislab;

import :concepts;
import :slab;
import std;

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-owning-memory)

namespace libmem {

/* ============================================================================
 * Slab node: element of two intrusive doubly-linked lists
 * ============================================================================ */

namespace detail {

template <std::size_t BlockSize, std::uint32_t BlocksPerSlab, std::size_t BlockAlign> struct slab_node {
    using slab_type = slab<BlockSize, BlocksPerSlab, BlockAlign>;

    struct link {
        slab_node* next{};
        slab_node* prev{};
    };

    slab_type allocator;
    /* `active_` or `full_`; relinked whenever the node fills or gains a free slot. */
    link space{};
    /* Every node, in iteration order; relinked only on grow and free, so live
     * iterators survive `space` moves. */
    link all{};
    std::uint32_t used{};
    void* raw_memory{};

    slab_node(void* mem, const std::size_t mem_size) noexcept : allocator{mem, mem_size}, raw_memory{mem} {}

    constexpr bool full() const noexcept { return used == BlocksPerSlab; }
};

} // namespace detail

/* ============================================================================
 * multislab: auto-expanding allocator
 * ============================================================================ */

/**
 * @brief Auto-expanding multi-slab allocator for fixed-size blocks.
 *
 * @tparam BlockSize     Stride from one block to the next, in bytes.
 * @tparam BlocksPerSlab Number of blocks per slab page.
 * @tparam Resource      Backing memory resource (must satisfy `memory_resource`).
 * @tparam Policy        Shrink policy (must satisfy `shrink_policy`).
 *
 * Every slab on `active_` has a free slot: a slab moves to `full_` on the
 * allocation that fills it and back on the next deallocation. Empty slabs are
 * released based on the shrink policy. Iteration follows a separate list, so
 * neither move disturbs a live iterator.
 */
export template <std::size_t BlockSize, std::uint32_t BlocksPerSlab, memory_resource Resource = default_resource, shrink_policy Policy = threshold_policy,
    std::size_t BlockAlign = default_alignment>
    requires valid_block_geometry<BlockSize, BlockAlign> && (BlocksPerSlab > 0)
class multislab {
    using node_type = detail::slab_node<BlockSize, BlocksPerSlab, BlockAlign>;
    using slab_type = typename node_type::slab_type;

    static constexpr std::size_t slab_memory_size{BlockSize * BlocksPerSlab};

public:
    static constexpr std::size_t block_size{BlockSize};
    static constexpr std::size_t block_alignment{BlockAlign};
    static constexpr std::uint32_t blocks_per_slab{BlocksPerSlab};

    /* ========================================================================
     * Construction / destruction
     * ======================================================================== */

    /** @brief Construct with default resource and policy. */
    constexpr multislab() noexcept = default;

    /** @brief Construct with a custom resource instance. */
    constexpr explicit multislab(Resource resource) noexcept : resource_{std::move(resource)} {}

    /** @brief Construct with custom resource and policy. */
    constexpr multislab(Resource resource, Policy policy) noexcept : resource_{std::move(resource)}, policy_{std::move(policy)} {}

    /** @brief Construct with a custom policy (default resource). */
    constexpr explicit multislab(Policy policy) noexcept : policy_{std::move(policy)} {}

    /** @brief Construct with a max slab limit. */
    constexpr explicit multislab(const std::uint32_t max_slabs) noexcept : max_slabs_{max_slabs} {}

    /** @brief Construct with max slabs, resource, and policy. */
    constexpr multislab(const std::uint32_t max_slabs, Resource resource, Policy policy) noexcept
        : resource_{std::move(resource)}, policy_{std::move(policy)}, max_slabs_{max_slabs} {}

    multislab(const multislab&) = delete;
    multislab& operator=(const multislab&) = delete;

    constexpr multislab(multislab&& other) noexcept
        : resource_{std::move(other.resource_)}, policy_{std::move(other.policy_)}, nodes_{std::exchange(other.nodes_, nullptr)},
          active_{std::exchange(other.active_, nullptr)}, full_{std::exchange(other.full_, nullptr)}, slab_count_{std::exchange(other.slab_count_, 0)},
          max_slabs_{other.max_slabs_}, empty_count_{std::exchange(other.empty_count_, 0)} {}

    constexpr multislab& operator=(multislab&& other) noexcept {
        if (this != &other) {
            destroy_all();
            resource_ = std::move(other.resource_);
            policy_ = std::move(other.policy_);
            nodes_ = std::exchange(other.nodes_, nullptr);
            active_ = std::exchange(other.active_, nullptr);
            full_ = std::exchange(other.full_, nullptr);
            slab_count_ = std::exchange(other.slab_count_, 0);
            max_slabs_ = other.max_slabs_;
            empty_count_ = std::exchange(other.empty_count_, 0);
        }
        return *this;
    }

    ~multislab() { destroy_all(); }

    /* ========================================================================
     * Allocation interface
     * ======================================================================== */

    /**
     * @brief Allocate a single block.
     * @return Pointer to the block, or `nullptr` if growth is not possible.
     */
    [[nodiscard]] void* allocate() { return allocate_raw().ptr; }

    /**
     * @brief Release a block previously obtained from `allocate()`.
     * @pre `ptr` was allocated from this multislab and has not been double-freed.
     */
    void deallocate(void* ptr) noexcept {
        assert(ptr != nullptr);

        node_type* node{find_owner(ptr)};
        assert(node != nullptr && "pointer not owned by this allocator");

        if (node->full()) [[unlikely]] {
            move_to_active(node);
        }

        node->allocator.deallocate(ptr);
        node->used--;

        /* Became empty: apply shrink policy. */
        if (node->used == 0) [[unlikely]] {
            empty_count_++;
            if (policy_.should_shrink(empty_count_, slab_count_)) {
                unlink_and_free(node);
            }
        }
    }

    /* ========================================================================
     * Queries
     * ======================================================================== */

    /** @brief Number of slab pages currently allocated. */
    constexpr std::uint32_t slab_count() const noexcept { return slab_count_; }

    /** @brief Maximum number of slabs (0 = unlimited). */
    constexpr std::uint32_t max_slabs() const noexcept { return max_slabs_; }

    /** @brief Number of currently empty (but retained) slabs. */
    constexpr std::uint32_t empty_slab_count() const noexcept { return empty_count_; }

    /** @brief Access the backing resource. */
    constexpr Resource& resource() noexcept { return resource_; }

    /** @brief Access the backing resource (const). */
    constexpr const Resource& resource() const noexcept { return resource_; }

    /** @brief Access the shrink policy. */
    constexpr Policy& policy() noexcept { return policy_; }

    /** @brief Access the shrink policy (const). */
    constexpr const Policy& policy() const noexcept { return policy_; }

    /* ========================================================================
     * Range interface: iterate over all allocated blocks
     * ======================================================================== */

    class iterator {
        friend class multislab;

    public:
        using difference_type = std::ptrdiff_t;
        using value_type = void*;

        constexpr iterator() noexcept = default;

    private:
        using slab_iterator = typename slab_type::iterator;

        /** @brief Positioned at the first allocated block at or after `node`. */
        constexpr explicit iterator(node_type* node) noexcept : node_{node} { settle(); }

        constexpr iterator(node_type* node, slab_iterator slab_iter) noexcept : node_{node}, slab_iter_{slab_iter} {}

    public:
        constexpr void* operator*() const noexcept { return *slab_iter_; }

        constexpr iterator& operator++() noexcept {
            ++slab_iter_;
            if (slab_iter_ == std::default_sentinel) {
                node_ = node_->all.next;
                settle();
            }
            return *this;
        }

        constexpr iterator operator++(int) noexcept {
            auto tmp{*this};
            ++(*this);
            return tmp;
        }

        constexpr bool operator==(std::default_sentinel_t) const noexcept { return node_ == nullptr; }

        friend constexpr bool operator==(std::default_sentinel_t s, const iterator& it) noexcept { return it == s; }

        /** @brief Equal when both are at the same block, or both at the end. */
        constexpr bool operator==(const iterator&) const noexcept = default;

    private:
        node_type* node_{};
        /* Default-constructed at the end, so defaulted equality holds there. */
        slab_iterator slab_iter_{};

        /** @brief Skip nodes with no allocated block; lands on a block or at the end. */
        constexpr void settle() noexcept {
            while (node_ && node_->used == 0) {
                node_ = node_->all.next;
            }
            slab_iter_ = node_ ? node_->allocator.begin() : slab_iterator{};
        }
    };

    /** @brief Begin iterator over all allocated blocks. */
    constexpr iterator begin() const noexcept { return iterator{nodes_}; }

    /** @brief Sentinel end. */
    static constexpr std::default_sentinel_t end() noexcept { return {}; }

    /**
     * @brief Build an iterator positioned at the allocated block `ptr`.
     *
     * Subsequent increments walk the remaining allocated blocks in the
     * same traversal order as `begin()`.
     *
     * @pre `ptr` was returned by this multislab's `allocate()` and is
     *      currently live.
     */
    iterator make_iterator(const void* ptr) const noexcept {
        node_type* node{find_owner(ptr)};
        if (!node) [[unlikely]] {
            return iterator{};
        }

        const auto base{reinterpret_cast<std::uintptr_t>(node->raw_memory)};
        const auto p{reinterpret_cast<std::uintptr_t>(ptr)};
        const auto index{static_cast<std::uint32_t>((p - base) / BlockSize)};

        return iterator_at(node, index);
    }

    /**
     * @brief Result of `allocate_at()`: the block pointer plus an iterator
     *        already positioned at it.
     *
     * `ptr == nullptr` means allocation failed, in which case `it` is a
     * default-constructed (end-equivalent) iterator.
     */
    struct allocation {
        void* ptr{};
        iterator it{};
    };

    /**
     * @brief Allocate a single block and return an iterator positioned at it.
     *
     * Equivalent to `allocate()` followed by `make_iterator(ptr)`, but O(1)
     * instead of O(S): allocation already knows the owning slab node and the
     * bit-index, so neither the `find_owner` scan nor the bitmap lookup is
     * needed. Prefer this whenever the caller wants the position.
     */
    [[nodiscard]] allocation allocate_at() {
        const auto raw{allocate_raw()};
        if (!raw.ptr) [[unlikely]] {
            return {};
        }
        return {raw.ptr, iterator_at(raw.node, raw.index)};
    }

    /* ========================================================================
     * Destroy
     * ======================================================================== */

    /**
     * @brief Destroy the multislab, releasing all slab nodes and their memory.
     * @post All previously allocated blocks are invalidated.
     */
    void destroy() noexcept { destroy_all(); }

private:
    Resource resource_{};
    Policy policy_{};
    node_type* nodes_{};
    node_type* active_{};
    node_type* full_{};
    std::uint32_t slab_count_{};
    std::uint32_t max_slabs_{};
    std::uint32_t empty_count_{};

    /* ========================================================================
     * Internal operations
     * ======================================================================== */

    /** @brief Result of `allocate_raw()`: block pointer plus its owning node and bit-index. */
    struct raw_allocation {
        void* ptr{};
        node_type* node{};
        std::uint32_t index{};
    };

    /**
     * @brief The actual allocation path, retaining the owning node and bit-index.
     *
     * `allocate()` discards them; `allocate_at()` uses them to build an iterator
     * without re-deriving what we already knew.
     */
    raw_allocation allocate_raw() {
        if (!active_ && !grow()) [[unlikely]] {
            return {};
        }

        node_type* node{active_};
        const auto alloc{node->allocator.allocate_at()};
        assert(alloc.ptr != nullptr && "a slab on the active list has no free slot");

        if (node->used++ == 0) {
            --empty_count_;
        }
        if (node->full()) {
            move_to_full(node);
        }
        return {alloc.ptr, node, alloc.index};
    }

    /** @brief Build an iterator at `index` within `node`. */
    iterator iterator_at(node_type* node, const std::uint32_t index) const noexcept { return iterator{node, node->allocator.make_iterator(index)}; }

    /**
     * @brief Alignment the slab's backing memory is taken at.
     *
     * Aligning the base is enough: `BlockSize` is a whole number of `BlockAlign`s, so every
     * block within the slab is `BlockAlign`-aligned too.
     */
    static constexpr std::size_t slab_alignment{BlockAlign};

    /**
     * @brief Take one slab's worth of backing memory, correctly aligned.
     *
     * @warning Must branch on the same condition as `free_slab_memory`: releasing an aligned
     *          allocation through the unaligned `deallocate` is undefined.
     */
    void* allocate_slab_memory() {
        if constexpr (aligned_memory_resource<Resource>) {
            return resource_.allocate(slab_memory_size, slab_alignment);
        } else {
            static_assert(slab_alignment <= default_alignment,
                "multislab: blocks aligned beyond default_alignment need an aligned_memory_resource, i.e. a resource with "
                "allocate(size, align) / deallocate(ptr, size, align).");
            return resource_.allocate(slab_memory_size);
        }
    }

    /** @brief Release memory from `allocate_slab_memory`; branches identically to it. */
    void free_slab_memory(void* ptr) noexcept {
        if constexpr (aligned_memory_resource<Resource>) {
            resource_.deallocate(ptr, slab_memory_size, slab_alignment);
        } else {
            resource_.deallocate(ptr, slab_memory_size);
        }
    }

    bool grow() {
        if (max_slabs_ && slab_count_ >= max_slabs_) {
            return false;
        }

        /* Allocate the node. */
        void* node_mem{resource_.allocate(sizeof(node_type))};
        if (!node_mem) [[unlikely]] {
            return false;
        }

        /* Allocate the slab backing memory. */
        void* slab_mem{allocate_slab_memory()};
        if (!slab_mem) [[unlikely]] {
            resource_.deallocate(node_mem, sizeof(node_type));
            return false;
        }

        auto* node{::new (node_mem) node_type{slab_mem, slab_memory_size}};
        push_front<&node_type::space>(active_, node);
        push_front<&node_type::all>(nodes_, node);

        slab_count_++;
        /* The new slab is empty. */
        empty_count_++;
        return true;
    }

    template <auto Link> static constexpr void push_front(node_type*& head, node_type* node) noexcept {
        node->*Link = {head, nullptr};
        if (head) {
            (head->*Link).prev = node;
        }
        head = node;
    }

    template <auto Link> static constexpr void unlink(node_type*& head, node_type* node) noexcept {
        const auto [next, prev]{node->*Link};
        if (prev) {
            (prev->*Link).next = next;
        } else {
            head = next;
        }
        if (next) {
            (next->*Link).prev = prev;
        }
    }

    void move_to_full(node_type* node) noexcept {
        unlink<&node_type::space>(active_, node);
        push_front<&node_type::space>(full_, node);
    }

    void move_to_active(node_type* node) noexcept {
        unlink<&node_type::space>(full_, node);
        push_front<&node_type::space>(active_, node);
    }

    /** @pre `node` is empty, hence on `active_`. */
    void unlink_and_free(node_type* node) noexcept {
        unlink<&node_type::space>(active_, node);
        unlink<&node_type::all>(nodes_, node);
        free_node(node);

        slab_count_--;
        empty_count_--;
    }

    void free_node(node_type* node) noexcept {
        void* raw{node->raw_memory};
        node->~node_type();
        free_slab_memory(raw);
        resource_.deallocate(node, sizeof(node_type));
    }

    node_type* find_owner(const void* ptr) const noexcept {
        const auto p{reinterpret_cast<std::uintptr_t>(ptr)};

        for (node_type* n{nodes_}; n; n = n->all.next) {
            const auto base{reinterpret_cast<std::uintptr_t>(n->raw_memory)};
            if (p >= base && p < base + slab_memory_size) {
                return n;
            }
        }
        return nullptr;
    }

    void destroy_all() noexcept {
        while (nodes_) {
            free_node(std::exchange(nodes_, nodes_->all.next));
        }
        active_ = nullptr;
        full_ = nullptr;
        slab_count_ = 0;
        empty_count_ = 0;
    }
};

/* Verify multislab::iterator satisfies input_iterator. */
static_assert(std::input_iterator<multislab<64, 64>::iterator>);

} // namespace libmem

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-owning-memory)
