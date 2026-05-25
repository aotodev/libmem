/**
 * @file typed_arena.cppm
 * @brief Bump allocator with destructor tracking for non-trivially-destructible types.
 *
 * `typed_arena` extends the bump-allocation model of `arena` to types whose
 * destructors have side effects (e.g. `std::string`, `std::vector`). A
 * lightweight intrusive destructor chain is maintained alongside the main
 * allocation cursor; on `reset()` or destruction, destructors are invoked in
 * reverse construction order.
 *
 * For trivially-destructible types no destructor entry is recorded, so the
 * overhead is identical to a plain `arena`.
 *
 * @code
 *     libmem::typed_arena scratch{1 << 16};
 *     auto* s = scratch.emplace<std::string>("hello");
 *     auto* v = scratch.emplace<std::vector<int>>(std::initializer_list<int>{1, 2, 3});
 *     scratch.reset();  // destructors called in reverse order
 * @endcode
 */
module;

#include <cassert>

export module libmem:typed_arena;

import :concepts;
import std;

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-owning-memory, cppcoreguidelines-pro-bounds-pointer-arithmetic)

namespace libmem {

/* ============================================================================
 * typed_arena — bump allocator with destructor tracking
 * ============================================================================ */

/**
 * @brief Linear allocator that supports non-trivially-destructible types.
 *
 * Allocation uses the same monotonic bump-pointer strategy as `arena`.
 * For each non-trivially-destructible object, a small `destructor_node` is
 * also bump-allocated and linked into a LIFO chain. On `reset()` or
 * destruction the chain is walked, calling each destructor in reverse
 * construction order before recycling the buffer.
 *
 * Trivially-destructible types incur no extra overhead — the destructor
 * chain is bypassed entirely.
 */
export class typed_arena {
public:
    /* ========================================================================
     * Construction / destruction
     * ======================================================================== */

    /**
     * @brief Construct over a caller-supplied buffer; the buffer must outlive
     *        the arena.
     */
    constexpr typed_arena(const std::span<std::byte> buffer, const std::size_t align = default_alignment) noexcept
        : begin_{buffer.data()}, end_{buffer.data() + buffer.size()}, cursor_{buffer.data()}, default_alignment_{align} {
        assert(align > 0 && (align & (align - 1)) == 0 && "alignment must be a power of two");
    }

    /**
     * @brief Construct an owning arena that heap-allocates `size` bytes.
     * @param size  Buffer size in bytes (must be > 0).
     * @param align Default alignment for untyped allocations.
     */
    explicit typed_arena(const std::size_t size, const std::size_t align = default_alignment)
        : begin_{new std::byte[size]}, end_{begin_ + size}, cursor_{begin_}, default_alignment_{align}, owns_buffer_{true} {
        assert(size > 0);
        assert(align > 0 && (align & (align - 1)) == 0 && "alignment must be a power of two");
    }

    typed_arena(const typed_arena&) = delete;
    typed_arena& operator=(const typed_arena&) = delete;

    constexpr typed_arena(typed_arena&& other) noexcept
        : begin_{std::exchange(other.begin_, nullptr)}, end_{std::exchange(other.end_, nullptr)}, cursor_{std::exchange(other.cursor_, nullptr)},
          default_alignment_{other.default_alignment_}, owns_buffer_{std::exchange(other.owns_buffer_, false)},
          dtor_head_{std::exchange(other.dtor_head_, nullptr)} {}

    constexpr typed_arena& operator=(typed_arena&& other) noexcept {
        if (this != &other) {
            destroy_all();
            release();
            begin_ = std::exchange(other.begin_, nullptr);
            end_ = std::exchange(other.end_, nullptr);
            cursor_ = std::exchange(other.cursor_, nullptr);
            default_alignment_ = other.default_alignment_;
            owns_buffer_ = std::exchange(other.owns_buffer_, false);
            dtor_head_ = std::exchange(other.dtor_head_, nullptr);
        }
        return *this;
    }

    ~typed_arena() {
        destroy_all();
        release();
    }

    /* ========================================================================
     * Untyped allocation interface (also satisfies `memory_resource`)
     * ======================================================================== */

    /**
     * @brief Bump-allocate `bytes` with `alignment`.
     * @return Pointer to the freshly bumped region, or `nullptr` if exhausted.
     * @note Raw allocations are not tracked — the caller is responsible for
     *       any required cleanup.
     */
    [[nodiscard]] void* allocate(const std::size_t bytes, const std::size_t alignment) noexcept {
        assert(alignment > 0 && (alignment & (alignment - 1)) == 0 && "alignment must be a power of two");

        const auto current{reinterpret_cast<std::uintptr_t>(cursor_)};
        const auto aligned{(current + alignment - 1) & ~static_cast<std::uintptr_t>(alignment - 1)};
        const auto next{aligned + bytes};
        if (next > reinterpret_cast<std::uintptr_t>(end_)) [[unlikely]] {
            return nullptr;
        }
        cursor_ = reinterpret_cast<std::byte*>(next);
        return reinterpret_cast<void*>(aligned);
    }

    /** @brief `memory_resource`-compatible allocate using the default alignment. */
    [[nodiscard]] void* allocate(const std::size_t bytes) noexcept { return allocate(bytes, default_alignment_); }

    /** @brief No-op deallocation — required by `memory_resource`. */
    void deallocate(void* /*ptr*/, const std::size_t /*bytes*/) noexcept {}

    /* ========================================================================
     * Typed allocation interface — supports ALL object types
     * ======================================================================== */

    /**
     * @brief Construct a `T` in place. Works for any constructible type.
     *
     * For trivially-destructible `T`, no destructor entry is recorded.
     * For non-trivially-destructible `T`, a `destructor_node` is bump-
     * allocated and linked into the LIFO chain.
     *
     * @return Pointer to the constructed `T`, or `nullptr` if the arena
     *         cannot satisfy the allocation.
     */
    template <typename T, typename... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] T* emplace(Args&&... args) {
        /* Pre-allocate space for the destructor node if T needs one. */
        void* dtor_mem{nullptr};
        if constexpr (!std::is_trivially_destructible_v<T>) {
            dtor_mem = allocate(sizeof(destructor_node), alignof(destructor_node));
            if (!dtor_mem) [[unlikely]] {
                return nullptr;
            }
        }

        /* Allocate space for T. */
        void* obj_mem{allocate(sizeof(T), alignof(T))};
        if (!obj_mem) [[unlikely]] {
            /* dtor_mem is wasted (arena doesn't support free), but safe. */
            return nullptr;
        }

        /* Construct T. If this throws, dtor_mem is wasted but the
         * destructor node was never linked, so no dangling call. */
        auto* obj{::new (obj_mem) T(std::forward<Args>(args)...)};

        /* Link the destructor node after successful construction. */
        if constexpr (!std::is_trivially_destructible_v<T>) {
            auto* node{::new (dtor_mem) destructor_node{&destroy_impl<T>, obj, dtor_head_}};
            dtor_head_ = node;
        }

        return obj;
    }

    /** @brief Copy-construct `value` into the arena. */
    template <typename T>
        requires std::copy_constructible<T>
    T* push_back(const T& value) {
        return emplace<T>(value);
    }

    /** @brief Move-construct `value` into the arena. */
    template <typename T>
        requires std::move_constructible<T>
    T* push_back(T&& value) {
        return emplace<T>(std::move(value));
    }

    /* ========================================================================
     * Lifetime management
     * ======================================================================== */

    /**
     * @brief Call all registered destructors in reverse order, then reset
     *        the cursor. The backing buffer is preserved for reuse.
     */
    void reset() noexcept {
        destroy_all();
        cursor_ = begin_;
    }

    /* ========================================================================
     * Queries
     * ======================================================================== */

    /** @brief Total buffer capacity in bytes. */
    constexpr std::size_t capacity() const noexcept { return static_cast<std::size_t>(end_ - begin_); }

    /** @brief Bytes consumed so far (includes destructor node overhead). */
    constexpr std::size_t used() const noexcept { return static_cast<std::size_t>(cursor_ - begin_); }

    /** @brief Bytes still available before exhaustion. */
    constexpr std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - cursor_); }

    /** @brief Whether the arena owns and will release its backing buffer. */
    constexpr bool owns_buffer() const noexcept { return owns_buffer_; }

    /** @brief Default alignment used by untyped `allocate(size)` calls. */
    constexpr std::size_t alignment() const noexcept { return default_alignment_; }

private:
    /**
     * @brief Intrusive LIFO node recording a single pending destructor.
     *
     * Trivially destructible itself (16–24 bytes), so it can live inside
     * the same bump-allocated buffer with no cleanup overhead.
     */
    struct destructor_node {
        void (*destroy)(void*) noexcept;
        void* ptr;
        destructor_node* next;
    };

    static_assert(std::is_trivially_destructible_v<destructor_node>);

    /** @brief Type-erased destructor thunk. */
    template <typename T> static void destroy_impl(void* ptr) noexcept { static_cast<T*>(ptr)->~T(); }

    std::byte* begin_{};
    std::byte* end_{};
    std::byte* cursor_{};
    std::size_t default_alignment_{default_alignment};
    bool owns_buffer_{false};
    destructor_node* dtor_head_{};

    /** @brief Walk the destructor chain in LIFO order. */
    void destroy_all() noexcept {
        for (destructor_node* node{dtor_head_}; node; node = node->next) {
            node->destroy(node->ptr);
        }
        dtor_head_ = nullptr;
    }

    void release() noexcept {
        if (owns_buffer_ && begin_) {
            delete[] begin_;
        }
        begin_ = nullptr;
        end_ = nullptr;
        cursor_ = nullptr;
        owns_buffer_ = false;
    }
};

} // namespace libmem

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-owning-memory, cppcoreguidelines-pro-bounds-pointer-arithmetic)
