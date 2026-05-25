/**
 * @file multislab.cppm
 * @brief Auto-expanding multi-slab allocator with hysteresis-based shrinking.
 *
 * Chains multiple `slab` instances together so that allocation never fails as
 * long as the backing memory resource can supply more pages. Empty slabs are
 * released according to a configurable hysteresis policy, keeping a small
 * reserve to avoid repeated grow/shrink cycles.
 *
 * The backing memory resource is injected via a concept-constrained template
 * parameter — zero virtual-dispatch overhead. Iteration over all allocated
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
 * Slab node — intrusive doubly-linked list element
 * ============================================================================ */

namespace detail {

template <std::size_t BlockSize, std::uint32_t BlocksPerSlab> struct slab_node {
    using slab_type = slab<BlockSize, BlocksPerSlab>;

    slab_type allocator;
    slab_node* next{};
    slab_node* prev{};
    std::uint32_t used{};
    void* raw_memory{};

    slab_node(void* mem, const std::size_t mem_size) noexcept : allocator{mem, mem_size}, raw_memory{mem} {}
};

} // namespace detail

/* ============================================================================
 * multislab — auto-expanding allocator
 * ============================================================================ */

/**
 * @brief Auto-expanding multi-slab allocator for fixed-size blocks.
 *
 * @tparam BlockSize     Size of each block (must satisfy `valid_block_size`).
 * @tparam BlocksPerSlab Number of blocks per slab page.
 * @tparam Resource      Backing memory resource (must satisfy `memory_resource`).
 * @tparam Policy        Shrink policy (must satisfy `shrink_policy`).
 *
 * Maintains two intrusive lists: `active` (slabs with free space) and `full`
 * (completely allocated slabs). On deallocation, full slabs are moved back to
 * active; empty slabs are released based on the shrink policy.
 */
export template <std::size_t BlockSize, std::uint32_t BlocksPerSlab, memory_resource Resource = default_resource, shrink_policy Policy = threshold_policy>
    requires valid_block_size<BlockSize> && (BlocksPerSlab > 0)
class multislab {
    using node_type = detail::slab_node<BlockSize, BlocksPerSlab>;
    using slab_type = typename node_type::slab_type;

    static constexpr std::size_t slab_memory_size{BlockSize * BlocksPerSlab};

public:
    static constexpr std::size_t block_size{BlockSize};
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
        : resource_{std::move(other.resource_)}, policy_{std::move(other.policy_)}, active_{std::exchange(other.active_, nullptr)},
          full_{std::exchange(other.full_, nullptr)}, slab_count_{std::exchange(other.slab_count_, 0)}, max_slabs_{other.max_slabs_},
          empty_count_{std::exchange(other.empty_count_, 0)} {}

    constexpr multislab& operator=(multislab&& other) noexcept {
        if (this != &other) {
            destroy_all();
            resource_ = std::move(other.resource_);
            policy_ = std::move(other.policy_);
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
    [[nodiscard]] void* allocate() {
        if (!active_) [[unlikely]] {
            if (!grow()) {
                return nullptr;
            }
        }

        node_type* node{active_};
        void* ptr{node->allocator.allocate()};

        if (!ptr) [[unlikely]] {
            /* This node is full — move it to the full list. */
            move_to_full(node);

            /* Grow a new slab and retry. */
            if (!grow()) {
                return nullptr;
            }
            node = active_;
            ptr = node->allocator.allocate();
        }

        if (ptr) [[likely]] {
            node->used++;
            if (node->used == 1) {
                /* Was empty, no longer empty. */
                if (empty_count_ > 0) {
                    --empty_count_;
                }
            }
        }
        return ptr;
    }

    /**
     * @brief Release a block previously obtained from `allocate()`.
     * @pre `ptr` was allocated from this multislab and has not been double-freed.
     */
    void deallocate(void* ptr) noexcept {
        assert(ptr != nullptr);

        node_type* node{find_owner(ptr)};
        assert(node != nullptr && "pointer not owned by this allocator");

        const bool was_full{node->used == BlocksPerSlab};

        node->allocator.deallocate(ptr);
        node->used--;

        /* If it was full, move back to the active list. */
        if (was_full) [[unlikely]] {
            move_to_active(node);
        }

        /* Became empty — apply shrink policy. */
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
     * Range interface — iterate over all allocated blocks
     * ======================================================================== */

    class iterator {
        friend class multislab;

    public:
        using difference_type = std::ptrdiff_t;
        using value_type = void*;

        constexpr iterator() noexcept = default;

        /** @brief Construct positioned at the first allocated block in `list`. */
        constexpr iterator(node_type* list_head, node_type* second_list) noexcept : current_node_{list_head}, second_list_{second_list} {
            advance_to_valid_node();
        }

    private:
        using slab_iterator = typename slab_type::iterator;

        /** @brief Construct from fully specified state (used by `multislab::make_iterator`). */
        constexpr iterator(node_type* node, node_type* second_list, slab_iterator slab_iter, bool on_second) noexcept
            : current_node_{node}, second_list_{second_list}, slab_iter_{slab_iter}, on_second_list_{on_second} {}

    public:
        constexpr void* operator*() const noexcept { return *slab_iter_; }

        constexpr iterator& operator++() noexcept {
            ++slab_iter_;
            if (slab_iter_ == std::default_sentinel) {
                /* Advance to the next slab node. */
                advance_node();
            }
            return *this;
        }

        constexpr iterator operator++(int) noexcept {
            auto tmp{*this};
            ++(*this);
            return tmp;
        }

        constexpr bool operator==(std::default_sentinel_t) const noexcept { return current_node_ == nullptr; }

        friend constexpr bool operator==(std::default_sentinel_t s, const iterator& it) noexcept { return it == s; }

        /**
         * @brief Iterator-to-iterator equality.
         *
         * Two iterators compare equal when they reference the same slab
         * node and either both reached their end state or both currently
         * dereference to the same allocated block.
         */
        constexpr bool operator==(const iterator& rhs) const noexcept {
            if (current_node_ != rhs.current_node_) {
                return false;
            }
            if (!current_node_) {
                return true;
            }
            return slab_iter_ == rhs.slab_iter_;
        }

    private:
        node_type* current_node_{};
        node_type* second_list_{};
        slab_iterator slab_iter_{};
        bool on_second_list_{false};

        constexpr void advance_to_valid_node() noexcept {
            /* Skip empty nodes, find first with content. */
            while (current_node_) {
                if (current_node_->used > 0) {
                    slab_iter_ = current_node_->allocator.begin();
                    if (slab_iter_ != std::default_sentinel) {
                        return;
                    }
                }
                current_node_ = current_node_->next;
                if (!current_node_ && !on_second_list_) {
                    current_node_ = second_list_;
                    on_second_list_ = true;
                }
            }
        }

        constexpr void advance_node() noexcept {
            current_node_ = current_node_->next;
            if (!current_node_ && !on_second_list_) {
                current_node_ = second_list_;
                on_second_list_ = true;
            }
            advance_to_valid_node();
        }
    };

    /** @brief Begin iterator over all allocated blocks (active list, then full list). */
    constexpr iterator begin() const noexcept { return iterator{active_, full_}; }

    /** @brief Sentinel end. */
    static constexpr std::default_sentinel_t end() noexcept { return {}; }

    /**
     * @brief Build an iterator positioned at the allocated block `ptr`.
     *
     * Subsequent increments walk the remaining allocated blocks in the
     * same traversal order as `begin()` (active list, then full list).
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

        /* Determine which intrusive list owns `node`. When in the `full_`
         * list the iterator is already at its "second list" stage. */
        bool in_full{false};
        for (node_type* n{full_}; n; n = n->next) {
            if (n == node) {
                in_full = true;
                break;
            }
        }

        return iterator{node, in_full ? nullptr : full_, node->allocator.make_iterator(index), in_full};
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
    node_type* active_{};
    node_type* full_{};
    std::uint32_t slab_count_{};
    std::uint32_t max_slabs_{};
    std::uint32_t empty_count_{};

    /* ========================================================================
     * Internal operations
     * ======================================================================== */

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
        void* slab_mem{resource_.allocate(slab_memory_size)};
        if (!slab_mem) [[unlikely]] {
            resource_.deallocate(node_mem, sizeof(node_type));
            return false;
        }

        /* Placement-new the node. */
        auto* node{::new (node_mem) node_type{slab_mem, slab_memory_size}};

        /* Prepend to the active list. */
        node->next = active_;
        node->prev = nullptr;
        if (active_) {
            active_->prev = node;
        }
        active_ = node;

        slab_count_++;
        /* The new slab is empty. */
        empty_count_++;
        return true;
    }

    void move_to_full(node_type* node) noexcept {
        /* Unlink from the active list. */
        if (node->prev) {
            node->prev->next = node->next;
        } else {
            active_ = node->next;
        }
        if (node->next) {
            node->next->prev = node->prev;
        }

        /* Prepend to the full list. */
        node->next = full_;
        node->prev = nullptr;
        if (full_) {
            full_->prev = node;
        }
        full_ = node;
    }

    void move_to_active(node_type* node) noexcept {
        /* Unlink from the full list. */
        if (node->prev) {
            node->prev->next = node->next;
        } else {
            full_ = node->next;
        }
        if (node->next) {
            node->next->prev = node->prev;
        }

        /* Prepend to the active list. */
        node->next = active_;
        node->prev = nullptr;
        if (active_) {
            active_->prev = node;
        }
        active_ = node;
    }

    void unlink_and_free(node_type* node) noexcept {
        /* Unlink from the active list. */
        if (node->prev) {
            node->prev->next = node->next;
        } else {
            active_ = node->next;
        }
        if (node->next) {
            node->next->prev = node->prev;
        }

        void* raw{node->raw_memory};
        node->~node_type();
        resource_.deallocate(raw, slab_memory_size);
        resource_.deallocate(node, sizeof(node_type));

        slab_count_--;
        empty_count_--;
    }

    node_type* find_owner(const void* ptr) const noexcept {
        const auto p{reinterpret_cast<std::uintptr_t>(ptr)};

        for (node_type* n{active_}; n; n = n->next) {
            const auto base{reinterpret_cast<std::uintptr_t>(n->raw_memory)};
            if (p >= base && p < base + slab_memory_size) {
                return n;
            }
        }
        for (node_type* n{full_}; n; n = n->next) {
            const auto base{reinterpret_cast<std::uintptr_t>(n->raw_memory)};
            if (p >= base && p < base + slab_memory_size) {
                return n;
            }
        }
        return nullptr;
    }

    void free_list(node_type* head) noexcept {
        while (head) {
            node_type* next{head->next};
            void* raw{head->raw_memory};
            head->~node_type();
            resource_.deallocate(raw, slab_memory_size);
            resource_.deallocate(head, sizeof(node_type));
            head = next;
        }
    }

    void destroy_all() noexcept {
        free_list(active_);
        free_list(full_);
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
