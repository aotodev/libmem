/**
 * @file arena.cppm
 * @brief Linear/scratch arena allocator (a.k.a. bump or region allocator).
 *
 * `arena` carves a single contiguous byte buffer with a monotonically advancing
 * cursor. Allocations are O(1), there is no per-allocation free, and the whole
 * region is reclaimed at once via `reset()` or at destruction. The allocator is
 * therefore well suited to short-lived scratch work where many small objects
 * share a common lifetime (per-frame data, parser scratch, intermediate
 * computations, …).
 *
 * The buffer can either be owned (heap-allocated internally) or borrowed
 * (provided by the caller as an `std::span<std::byte>`). Element placement
 * goes through `emplace<T>` / `push_back<T>`; both require `T` to be
 * trivially destructible since the arena never invokes destructors.
 *
 * `arena` also satisfies the `memory_resource` concept, so it can drop into
 * any allocator slot that accepts one (e.g. as the backing store of
 * `multislab` / `pool`).
 *
 * @code
 *     libmem::arena scratch{1 << 20};
 *     auto* v = scratch.emplace<my_vertex>(1.f, 2.f, 3.f);
 *     auto* arr = scratch.allocate_n<float>(64);
 *     scratch.reset();
 * @endcode
 */
module;

#include <cassert>

export module libmem:arena;

import :concepts;
import std;

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-owning-memory, cppcoreguidelines-pro-bounds-pointer-arithmetic)

namespace libmem {

/* ============================================================================
 * arena — monotonically advancing bump allocator
 * ============================================================================ */

/**
 * @brief Linear scratch allocator over a single contiguous byte buffer.
 *
 * Allocation never invokes a destructor — `T` must therefore be trivially
 * destructible for any typed entry point (`emplace`, `push_back`,
 * `allocate_n`). The allocator can either own its buffer (heap-allocated via
 * `operator new[]`) or wrap a user-supplied buffer.
 */
export class arena {
public:
    /* ========================================================================
     * Construction / destruction
     * ======================================================================== */

    /**
     * @brief Construct over a caller-supplied buffer; the buffer must outlive
     *        the arena.
     * @param buffer            Non-owning byte buffer to bump-allocate from.
     * @param align             Alignment applied to untyped `allocate(n)`
     *                          calls that do not pass an explicit alignment.
     */
    constexpr arena(const std::span<std::byte> buffer, const std::size_t align = default_alignment) noexcept
        : begin_{buffer.data()}, end_{buffer.data() + buffer.size()}, cursor_{buffer.data()}, default_alignment_{align}, owns_buffer_{false} {
        assert(align > 0 && (align & (align - 1)) == 0 && "alignment must be a power of two");
    }

    /**
     * @brief Construct an owning arena that heap-allocates `size` bytes.
     * @param size  Buffer size in bytes (must be > 0).
     * @param align Alignment applied to untyped `allocate(n)` calls that do
     *              not pass an explicit alignment.
     */
    explicit arena(const std::size_t size, const std::size_t align = default_alignment)
        : begin_{new std::byte[size]}, end_{begin_ + size}, cursor_{begin_}, default_alignment_{align}, owns_buffer_{true} {
        assert(size > 0);
        assert(align > 0 && (align & (align - 1)) == 0 && "alignment must be a power of two");
    }

    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;

    constexpr arena(arena&& other) noexcept
        : begin_{std::exchange(other.begin_, nullptr)}, end_{std::exchange(other.end_, nullptr)}, cursor_{std::exchange(other.cursor_, nullptr)},
          default_alignment_{other.default_alignment_}, owns_buffer_{std::exchange(other.owns_buffer_, false)} {}

    constexpr arena& operator=(arena&& other) noexcept {
        if (this != &other) {
            release();
            begin_ = std::exchange(other.begin_, nullptr);
            end_ = std::exchange(other.end_, nullptr);
            cursor_ = std::exchange(other.cursor_, nullptr);
            default_alignment_ = other.default_alignment_;
            owns_buffer_ = std::exchange(other.owns_buffer_, false);
        }
        return *this;
    }

    ~arena() { release(); }

    /* ========================================================================
     * Untyped allocation interface (also satisfies `memory_resource`)
     * ======================================================================== */

    /**
     * @brief Bump-allocate `bytes` with `alignment`.
     * @return Pointer to the freshly bumped region, or `nullptr` if exhausted.
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

    /**
     * @brief No-op deallocation — required by `memory_resource`; the cursor
     *        only moves backwards via `reset()`.
     */
    void deallocate(void* /*ptr*/, const std::size_t /*bytes*/) noexcept {}

    /* ========================================================================
     * Typed allocation interface
     * ======================================================================== */

    /**
     * @brief Reserve uninitialised storage for `count` instances of `T`.
     *
     * The returned memory is properly aligned for `T` but **not initialised**.
     * Useful when the caller manages construction itself (e.g. parallel
     * placement-new from a writer thread).
     */
    template <typename T>
        requires std::is_trivially_destructible_v<T>
    [[nodiscard]] T* allocate_n(const std::size_t count) noexcept {
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }

    /**
     * @brief Construct a single `T` in place inside the arena.
     * @return Pointer to the newly constructed `T`, or `nullptr` if the arena
     *         is exhausted.
     */
    template <typename T, typename... Args>
        requires std::is_trivially_destructible_v<T> && std::constructible_from<T, Args...>
    [[nodiscard]] T* emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        void* mem{allocate(sizeof(T), alignof(T))};
        if (!mem) [[unlikely]] {
            return nullptr;
        }
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    /** @brief Copy-construct `value` into the arena. */
    template <typename T>
        requires std::is_trivially_destructible_v<T> && std::copy_constructible<T>
    T* push_back(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return emplace<T>(value);
    }

    /** @brief Move-construct `value` into the arena. */
    template <typename T>
        requires std::is_trivially_destructible_v<T> && std::move_constructible<T>
    T* push_back(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return emplace<T>(std::move(value));
    }

    /* ========================================================================
     * Lifetime management
     * ======================================================================== */

    /**
     * @brief Reset the cursor — all previously handed-out pointers become
     *        invalid but the backing buffer is preserved for reuse.
     */
    constexpr void reset() noexcept { cursor_ = begin_; }

    /* ========================================================================
     * Queries
     * ======================================================================== */

    /** @brief Total buffer capacity in bytes. */
    constexpr std::size_t capacity() const noexcept { return static_cast<std::size_t>(end_ - begin_); }

    /** @brief Bytes consumed so far. */
    constexpr std::size_t used() const noexcept { return static_cast<std::size_t>(cursor_ - begin_); }

    /** @brief Bytes still available before exhaustion. */
    constexpr std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - cursor_); }

    /** @brief Whether the arena owns and will release its backing buffer. */
    constexpr bool owns_buffer() const noexcept { return owns_buffer_; }

    /** @brief Default alignment used by untyped `allocate(size)` calls. */
    constexpr std::size_t alignment() const noexcept { return default_alignment_; }

    /** @brief Read-only view of the bytes currently in use. */
    constexpr std::span<const std::byte> as_bytes() const noexcept { return {begin_, used()}; }

private:
    std::byte* begin_{};
    std::byte* end_{};
    std::byte* cursor_{};
    std::size_t default_alignment_{default_alignment};
    bool owns_buffer_{false};

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
