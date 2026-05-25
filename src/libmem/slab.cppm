/**
 * @file slab.cppm
 * @brief Fixed-size block slab allocator with compile-time bitmap sizing.
 *
 * A zero-overhead slab allocator that carves a user-provided contiguous memory
 * region into fixed-size blocks, tracked by an inline bitmap. Block size and
 * maximum capacity are template parameters, enabling the compiler to reduce
 * index-to-pointer arithmetic to shifts and masks.
 *
 * @code
 *     alignas(64) std::byte storage[4096 * 8];
 *     libmem::slab<4096, 8> pool{storage, sizeof(storage)};
 *     void* blk = pool.allocate();
 * @endcode
 */
module;

#include <cassert>

export module libmem:slab;

import :concepts;
import std;

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)

namespace libmem {

/* ============================================================================
 * Bitmap constants
 * ============================================================================ */

/** @brief Number of bits in a single bitmap word. */
inline constexpr std::uint32_t bitmap_word_bits{64};

/* ============================================================================
 * Bitmap utilities — constexpr-friendly, zero-overhead bit manipulation
 * ============================================================================ */

namespace detail {

/** @brief Compute the number of bitmap words needed for `block_count` blocks. */
consteval std::size_t words_for(const std::uint32_t block_count) noexcept {
    return (block_count + bitmap_word_bits - 1) / bitmap_word_bits;
}

/**
 * @brief Find the first free (zero) bit across `Words` bitmap words.
 * @return Index of the free bit, or -1 if all bits are set.
 */
template <std::size_t Words> constexpr std::int32_t bitmap_find_free(const std::array<std::uint64_t, Words>& bitmap, const std::uint32_t capacity) noexcept {
    for (std::size_t i{0}; i < Words; ++i) {
        if (~bitmap[i]) {
            const auto bit = static_cast<std::uint32_t>(std::countr_zero(static_cast<std::uint64_t>(~bitmap[i])));
            const std::uint32_t index{static_cast<std::uint32_t>(i) * bitmap_word_bits + bit};
            if (index < capacity) {
                return static_cast<std::int32_t>(index);
            }
        }
    }
    return -1;
}

/** @brief Test whether a bit is set. */
template <std::size_t Words> constexpr bool bitmap_test(const std::array<std::uint64_t, Words>& bitmap, const std::uint32_t index) noexcept {
    return (bitmap[index / bitmap_word_bits] & (1ULL << (index & (bitmap_word_bits - 1)))) != 0;
}

/** @brief Set a bit. */
template <std::size_t Words> constexpr void bitmap_set(std::array<std::uint64_t, Words>& bitmap, const std::uint32_t index) noexcept {
    bitmap[index / bitmap_word_bits] |= (1ULL << (index & (bitmap_word_bits - 1)));
}

/** @brief Clear a bit. */
template <std::size_t Words> constexpr void bitmap_clear(std::array<std::uint64_t, Words>& bitmap, const std::uint32_t index) noexcept {
    bitmap[index / bitmap_word_bits] &= ~(1ULL << (index & (bitmap_word_bits - 1)));
}

} // namespace detail

/* ============================================================================
 * slab — fixed-size block allocator with compile-time capacity
 * ============================================================================ */

/**
 * @brief Fixed-size slab allocator backed by user-provided memory.
 *
 * @tparam BlockSize  Size of each block in bytes (must satisfy `valid_block_size`).
 * @tparam MaxBlocks  Maximum number of blocks the slab can manage.
 *
 * The bitmap is stored inline (no heap allocation). All pointer arithmetic
 * reduces to compile-time constants where `BlockSize` is a power of two.
 */
export template <std::size_t BlockSize, std::uint32_t MaxBlocks>
    requires valid_block_size<BlockSize> && (MaxBlocks > 0)
class slab {
    static constexpr std::size_t bitmap_words{detail::words_for(MaxBlocks)};

public:
    static constexpr std::size_t block_size{BlockSize};
    static constexpr std::uint32_t capacity{MaxBlocks};
    static constexpr std::size_t required_memory{BlockSize * MaxBlocks};

    /**
     * @brief Construct from a user-provided memory region.
     * @param memory      Base pointer to the backing storage.
     * @param memory_size Size of the backing storage (must be >= `required_memory`).
     * @pre `memory != nullptr && memory_size >= required_memory`.
     */
    constexpr slab(void* memory, const std::size_t memory_size) noexcept
        : memory_{static_cast<std::byte*>(memory)}, block_count_{static_cast<std::uint32_t>(memory_size / BlockSize)} {
        assert(memory != nullptr);
        assert(memory_size >= BlockSize);
        assert(block_count_ <= MaxBlocks);
    }

    /**
     * @brief Allocate a single block.
     * @return Pointer to the allocated block, or `nullptr` if full.
     */
    [[nodiscard]] constexpr void* allocate() noexcept {
        const auto index{detail::bitmap_find_free(bitmap_, block_count_)};
        if (index < 0) [[unlikely]] {
            return nullptr;
        }
        detail::bitmap_set(bitmap_, static_cast<std::uint32_t>(index));
        return index_to_ptr(static_cast<std::uint32_t>(index));
    }

    /**
     * @brief Release a previously allocated block.
     * @param ptr Pointer previously returned by `allocate()`.
     * @pre `ptr` was returned by this slab's `allocate()` and has not been double-freed.
     */
    constexpr void deallocate(void* ptr) noexcept {
        assert(ptr != nullptr);
        assert(owns(ptr));

        const auto index{ptr_to_index(ptr)};
        assert(detail::bitmap_test(bitmap_, index) && "double-free detected");

        detail::bitmap_clear(bitmap_, index);
    }

    /** @brief Test whether `ptr` belongs to this slab's memory region. */
    constexpr bool owns(const void* ptr) const noexcept {
        const auto p{reinterpret_cast<std::uintptr_t>(ptr)};
        const auto base{reinterpret_cast<std::uintptr_t>(memory_)};
        const auto end{base + static_cast<std::uintptr_t>(block_count_) * BlockSize};
        if (p < base || p >= end) {
            return false;
        }
        return (p - base) % BlockSize == 0;
    }

    /**
     * @brief Reset the allocator — all blocks become available.
     * @note Does not touch the backing memory contents.
     */
    constexpr void reset() noexcept { bitmap_.fill(0); }

    /** @brief Number of currently allocated blocks. */
    constexpr std::uint32_t used_count() const noexcept {
        std::uint32_t count{0};
        for (const auto word : bitmap_) {
            count += static_cast<std::uint32_t>(std::popcount(word));
        }
        return count;
    }

    /** @brief True when no blocks are allocated. */
    constexpr bool empty() const noexcept {
        for (const auto word : bitmap_) {
            if (word) {
                return false;
            }
        }
        return true;
    }

    /** @brief True when all blocks are allocated. */
    constexpr bool full() const noexcept { return used_count() == block_count_; }

    /** @brief Actual number of blocks managed (may be < MaxBlocks if memory was smaller). */
    constexpr std::uint32_t block_count() const noexcept { return block_count_; }

    /** @brief Access the raw bitmap for external iteration. */
    constexpr std::span<const std::uint64_t, bitmap_words> bitmap() const noexcept { return bitmap_; }

    /** @brief Base pointer of the managed memory. */
    constexpr std::byte* data() const noexcept { return memory_; }

    /* ========================================================================
     * Iterator — walks allocated blocks via bitmap scanning
     * ======================================================================== */

    class iterator {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = void*;

        constexpr iterator() noexcept = default;

        constexpr iterator(const slab* s, std::uint32_t word_idx, std::uint64_t word) noexcept : slab_{s}, word_idx_{word_idx}, word_{word} {}

        constexpr void* operator*() const noexcept {
            const auto bit{static_cast<std::uint32_t>(std::countr_zero(word_))};
            const std::uint32_t index{word_idx_ * bitmap_word_bits + bit};
            return const_cast<void*>(static_cast<const void*>(slab_->memory_ + index * BlockSize));
        }

        constexpr iterator& operator++() noexcept {
            /* Clear the lowest set bit. */
            word_ &= word_ - 1;
            if (!word_) {
                advance_word();
            }
            return *this;
        }

        constexpr iterator operator++(int) noexcept {
            auto tmp{*this};
            ++(*this);
            return tmp;
        }

        constexpr bool operator==(std::default_sentinel_t) const noexcept { return slab_ == nullptr || (!word_ && word_idx_ >= bitmap_words); }

        friend constexpr bool operator==(std::default_sentinel_t s, const iterator& it) noexcept { return it == s; }

        /**
         * @brief Iterator-to-iterator equality.
         *
         * Two iterators compare equal when they reference the same slab and
         * either both reached their end state or both currently dereference
         * to the same allocated block.
         */
        constexpr bool operator==(const iterator& rhs) const noexcept {
            if (slab_ != rhs.slab_) {
                return false;
            }
            const bool lhs_end{slab_ == nullptr || (!word_ && word_idx_ >= bitmap_words)};
            const bool rhs_end{rhs.slab_ == nullptr || (!rhs.word_ && rhs.word_idx_ >= bitmap_words)};
            if (lhs_end || rhs_end) {
                return lhs_end && rhs_end;
            }
            if (word_idx_ != rhs.word_idx_) {
                return false;
            }
            return std::countr_zero(word_) == std::countr_zero(rhs.word_);
        }

    private:
        const slab* slab_{};
        std::uint32_t word_idx_{};
        std::uint64_t word_{};

        constexpr void advance_word() noexcept {
            ++word_idx_;
            while (word_idx_ < bitmap_words) {
                word_ = slab_->bitmap_[word_idx_];
                if (word_) {
                    return;
                }
                ++word_idx_;
            }
        }
    };

    /** @brief Begin iterator over allocated blocks. */
    constexpr iterator begin() const noexcept {
        for (std::uint32_t i{0}; i < bitmap_words; ++i) {
            if (bitmap_[i]) {
                return iterator{this, i, bitmap_[i]};
            }
        }
        return iterator{this, static_cast<std::uint32_t>(bitmap_words), 0};
    }

    /** @brief Sentinel marking the end of iteration. */
    static constexpr std::default_sentinel_t end() noexcept { return {}; }

    /**
     * @brief Construct an iterator positioned at the allocated block whose
     *        bit-index is `index`. Iteration from this point yields that
     *        block first, followed by the remaining allocated blocks in
     *        ascending order.
     * @pre `index < capacity` and the bit at `index` is set.
     */
    constexpr iterator make_iterator(const std::uint32_t index) const noexcept {
        const std::uint32_t word_idx{index / bitmap_word_bits};
        const std::uint32_t bit{index & (bitmap_word_bits - 1)};
        if (word_idx >= bitmap_words) [[unlikely]] {
            return iterator{this, static_cast<std::uint32_t>(bitmap_words), 0};
        }
        /* Mask out bits strictly below `bit` so the iterator visits this
         * index first. */
        const std::uint64_t masked{bitmap_[word_idx] & (~std::uint64_t{0} << bit)};
        if (masked) {
            return iterator{this, word_idx, masked};
        }
        /* Empty word after masking → advance to the next non-empty word. */
        for (std::uint32_t i{word_idx + 1}; i < bitmap_words; ++i) {
            if (bitmap_[i]) {
                return iterator{this, i, bitmap_[i]};
            }
        }
        return iterator{this, static_cast<std::uint32_t>(bitmap_words), 0};
    }

private:
    std::byte* memory_{};
    std::uint32_t block_count_{};
    std::array<std::uint64_t, bitmap_words> bitmap_{};

    constexpr void* index_to_ptr(const std::uint32_t index) const noexcept { return static_cast<void*>(memory_ + static_cast<std::size_t>(index) * BlockSize); }

    constexpr std::uint32_t ptr_to_index(const void* ptr) const noexcept {
        return static_cast<std::uint32_t>((reinterpret_cast<std::uintptr_t>(ptr) - reinterpret_cast<std::uintptr_t>(memory_)) / BlockSize);
    }
};

/* Verify slab::iterator satisfies input_iterator. */
static_assert(std::input_iterator<slab<64, 64>::iterator>);

} // namespace libmem

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
