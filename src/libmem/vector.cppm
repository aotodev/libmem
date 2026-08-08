/**
 * @file vector.cppm
 * @brief Contiguous growable sequence over a `storage`, including the small-buffer variant.
 *
 * @code
 *     libmem::vector<node> nodes{};                     // heap, geometric growth
 *     libmem::small_vector<node, 8> few{};              // 8 inline, spills past that
 *     libmem::inline_vector<node, 8> bounded{};         // 8 inline, never grows
 *
 *     libmem::arena scratch{1 << 20};
 *     libmem::vector<node, libmem::resource_ref<libmem::arena>> out{libmem::resource_ref{scratch}};
 * @endcode
 *
 * @section vector_failure Failure is a return value, not an exception
 *
 * `emplace_back` and `push_back` return a `T*` that is null when the storage could
 * not make room, `reserve` and `resize` return `bool`, and there is no throwing
 * `at()`. On a fixed extent that null means full.
 *
 * `T`'s own constructors may throw. When one does, the vector is left exactly as
 * it was.
 *
 * @section vector_moves Moving
 *
 * Movable when the storage is `relocatable` (a pointer steal) or
 * `transferable_storage` (`small_storage`: a spilled block is stolen, an unspilled
 * one falls back to moving the elements into the destination's inline slots).
 * `inline_vector` is neither and does not move.
 *
 * Copying is deleted throughout, as it is for every other libmem container.
 */
module;

#include <cassert>

export module libmem:vector;

import :concepts;
import :storage;
import std;

namespace libmem {

/**
 * @brief Contiguous sequence of `T` over an arbitrary `storage`.
 *
 * @tparam Storage Storage supplying the slots. A `growable_storage` gives a
 *                 `std::vector`-shaped container; a fixed-extent one gives a
 *                 bounded sequence whose `push_back` reports full instead of
 *                 growing.
 *
 * Prefer the `vector` / `small_vector` / `inline_vector` / `fixed_vector` aliases
 * below over naming this directly.
 *
 * @warning Growth relocates, so every pointer, reference, and iterator into the
 *          vector is invalidated by an insertion that grows it. `erase`
 *          invalidates everything from the erased position onwards;
 *          `erase_unordered` invalidates only the erased position and the last.
 */
export template <storage Storage>
    requires std::is_object_v<typename Storage::value_type>
class basic_vector {
public:
    /* ========================================================================
     * Member types
     * ======================================================================== */

    using storage_type = Storage;
    using value_type = typename Storage::value_type;
    using size_type = typename Storage::size_type;
    using difference_type = std::ptrdiff_t;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using iterator = value_type*;
    using const_iterator = const value_type*;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    /** @brief Whether an insertion can grow, or fails once the fixed extent is full. */
    static constexpr bool growable{growable_storage<Storage>};

    /** @brief The extent, or `dynamic_extent` when it is a runtime property. */
    static constexpr size_type static_capacity{Storage::static_capacity};

    /**
     * @brief Whether the vector can be moved; see @ref vector_moves.
     *
     * @note The transferable branch also needs a default-constructible storage:
     *       the destination must exist before it can adopt anything.
     */
    static constexpr bool relocatable{Storage::relocatable || (transferable_storage<Storage> && std::default_initializable<Storage>)};

    /* ========================================================================
     * Construction / destruction
     * ======================================================================== */

    basic_vector()
        requires std::default_initializable<Storage>
    = default;

    /**
     * @brief Construct the storage from `args`, typically a resource.
     *
     * @code
     *     libmem::arena scratch{1 << 20};
     *     libmem::vector<node, libmem::resource_ref<libmem::arena>> out{libmem::resource_ref{scratch}};
     * @endcode
     */
    template <typename... Args>
        requires(sizeof...(Args) > 0) && (!std::same_as<std::remove_cvref_t<Args>, basic_vector> && ...) && std::constructible_from<Storage, Args...>
    explicit basic_vector(Args&&... args) : store_{std::forward<Args>(args)...} {}

    /**
     * @brief Construct from a range, so `std::ranges::to` can build one.
     *
     * @note Appends what fits. A fixed extent, or a resource that runs out, leaves
     *       the tail of `range` behind rather than throwing; use `append_range` on
     *       an existing vector when the count matters.
     */
    template <std::ranges::input_range R>
        requires std::constructible_from<value_type, std::ranges::range_reference_t<R>> && std::default_initializable<Storage>
    basic_vector(std::from_range_t, R&& range) {
        static_cast<void>(append_range(std::forward<R>(range)));
    }

    basic_vector(const basic_vector&) = delete;
    basic_vector& operator=(const basic_vector&) = delete;

    /** @brief Move by transferring the storage; the elements never move. */
    basic_vector(basic_vector&& other) noexcept
        requires Storage::relocatable
        : store_{std::move(other.store_)}, size_{std::exchange(other.size_, 0)} {}

    /**
     * @brief Move a small-buffer vector: steal the spilled block, or move the elements.
     *
     * @throws Whatever `T`'s move constructor throws, in which case `other` still
     *         holds every element and this vector is empty.
     */
    basic_vector(basic_vector&& other) noexcept(std::is_nothrow_move_constructible_v<value_type>)
        requires(!Storage::relocatable) && transferable_storage<Storage> && std::default_initializable<Storage> && std::move_constructible<value_type>
    {
        take_over(other);
    }

    basic_vector& operator=(basic_vector&& other) noexcept
        requires Storage::relocatable
    {
        if (this != &other) {
            clear();
            store_ = std::move(other.store_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    basic_vector& operator=(basic_vector&& other) noexcept(std::is_nothrow_move_constructible_v<value_type>)
        requires(!Storage::relocatable) && transferable_storage<Storage> && std::move_constructible<value_type>
    {
        if (this != &other) {
            clear();
            take_over(other);
        }
        return *this;
    }

    ~basic_vector() { clear(); }

    /* ========================================================================
     * Capacity
     * ======================================================================== */

    constexpr size_type size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr size_type capacity() const noexcept { return store_.capacity(); }

    /**
     * @brief Grow to hold at least `n` elements.
     * @return `false` when the storage could not supply the space, leaving the
     *         vector untouched. Always `false` past a fixed extent.
     * @throws Whatever `T`'s move constructor throws, having changed nothing.
     */
    bool reserve(const size_type n) {
        if (store_.capacity() >= n) {
            return true;
        }
        if constexpr (growable) {
            return relocate_grow(store_, n, size_);
        } else {
            return false;
        }
    }

    /**
     * @brief Destroy elements at the back until `size() <= n`; a no-op when it already is.
     *
     * Shrink-only, so unlike `resize` it requires nothing of `T`.
     */
    void truncate(const size_type n) noexcept {
        if (n < size_) {
            std::destroy_n(store_.data() + n, size_ - n);
            size_ = n;
        }
    }

    /**
     * @brief Value-initialise or destroy elements at the back until `size() == n`.
     * @return `false` when growing to `n` was not possible, leaving the vector untouched.
     */
    bool resize(const size_type n)
        requires std::default_initializable<value_type>
    {
        return resize_to(n, [](value_type* first, const size_type count) { std::uninitialized_value_construct_n(first, count); });
    }

    /** @brief As `resize(n)`, filling any new elements with copies of `value`. */
    bool resize(const size_type n, const value_type& value)
        requires std::copy_constructible<value_type>
    {
        return resize_to(n, [&value](value_type* first, const size_type count) { std::uninitialized_fill_n(first, count, value); });
    }

    /* ========================================================================
     * Element access
     * ======================================================================== */

    constexpr auto* data(this auto&& self) noexcept { return self.store_.data(); }

    /**
     * @brief The element at `index`.
     * @pre `index < size()`.
     */
    constexpr auto& operator[](this auto&& self, const size_type index) noexcept {
        assert(index < self.size_ && "vector: index out of range");
        return self.data()[index];
    }

    /** @pre `!empty()`. */
    constexpr auto& front(this auto&& self) noexcept {
        assert(self.size_ > 0 && "vector: front() on an empty vector");
        return *self.data();
    }

    /** @pre `!empty()`. */
    constexpr auto& back(this auto&& self) noexcept {
        assert(self.size_ > 0 && "vector: back() on an empty vector");
        return self.data()[self.size_ - 1];
    }

    /* ========================================================================
     * Iteration: contiguous, so plain pointers suffice
     * ======================================================================== */

    constexpr auto* begin(this auto&& self) noexcept { return self.data(); }
    constexpr auto* end(this auto&& self) noexcept { return self.data() + self.size_; }

    constexpr const_iterator cbegin() const noexcept { return begin(); }
    constexpr const_iterator cend() const noexcept { return end(); }

    constexpr auto rbegin(this auto&& self) noexcept { return std::reverse_iterator{self.end()}; }
    constexpr auto rend(this auto&& self) noexcept { return std::reverse_iterator{self.begin()}; }

    /** @brief The elements as one contiguous view. */
    constexpr auto elements(this auto&& self) noexcept { return std::span{self.data(), self.size_}; }

    /* ========================================================================
     * Modifiers
     * ======================================================================== */

    /**
     * @brief Construct an element in place at the back.
     * @return A pointer to it, or `nullptr` when the storage could not make room.
     * @throws Whatever `T`'s constructor throws, having changed nothing but a
     *         capacity that may already have grown.
     */
    template <typename... Args>
        requires std::constructible_from<value_type, Args...>
    value_type* emplace_back(Args&&... args) {
        if (!reserve(size_ + 1)) {
            return nullptr;
        }

        value_type* slot{store_.data() + size_};
        std::construct_at(slot, std::forward<Args>(args)...);
        ++size_;

        return slot;
    }

    /** @brief Copy an element onto the back; `nullptr` when there is no room. */
    value_type* push_back(const value_type& value)
        requires std::copy_constructible<value_type>
    {
        return emplace_back(value);
    }

    /** @brief Move an element onto the back; `nullptr` when there is no room. */
    value_type* push_back(value_type&& value)
        requires std::move_constructible<value_type>
    {
        return emplace_back(std::move(value));
    }

    /**
     * @brief Append every element of `range`.
     * @return How many were appended, which is short of the range's length when
     *         the storage ran out.
     */
    template <std::ranges::input_range R>
        requires std::constructible_from<value_type, std::ranges::range_reference_t<R>>
    size_type append_range(R&& range) {
        /* One growth rather than log(n) of them when the length is known up front.
         * A failure here is not fatal: the loop below still appends what fits. */
        if constexpr (std::ranges::sized_range<R>) {
            static_cast<void>(reserve(size_ + static_cast<size_type>(std::ranges::size(range))));
        }

        size_type added{};
        for (auto&& element : range) {
            if (!emplace_back(std::forward<decltype(element)>(element))) {
                break;
            }
            ++added;
        }
        return added;
    }

    /**
     * @brief Destroy the last element.
     * @pre `!empty()`.
     */
    void pop_back() noexcept {
        assert(size_ > 0 && "vector: pop_back() on an empty vector");
        std::destroy_at(store_.data() + --size_);
    }

    /**
     * @brief Erase the element at `pos`, shifting the tail down to close the gap.
     * @return An iterator to the element that took its place, which is `end()`
     *         when the erased one was last.
     */
    iterator erase(const const_iterator pos)
        requires std::is_move_assignable_v<value_type>
    {
        assert(pos >= cbegin() && pos < cend() && "vector: erase position out of range");
        return erase(pos, pos + 1);
    }

    /** @brief Erase `[first, last)`, shifting the tail down to close the gap. */
    iterator erase(const const_iterator first, const const_iterator last)
        requires std::is_move_assignable_v<value_type>
    {
        assert(first >= cbegin() && last <= cend() && first <= last && "vector: erase range out of range");

        value_type* slots{store_.data()};
        const size_type at{static_cast<size_type>(first - cbegin())};
        const size_type removed{static_cast<size_type>(last - first)};

        if (removed > 0) {
            std::move(slots + at + removed, slots + size_, slots + at);
            std::destroy_n(slots + (size_ - removed), removed);
            size_ -= removed;
        }

        return slots + at;
    }

    /**
     * @brief Erase the element at `pos` by moving the last one into its place.
     *
     * O(1), and does not preserve the order.
     *
     * @return An iterator to the element that took its place, which is `end()`
     *         when the erased one was last.
     */
    iterator erase_unordered(const const_iterator pos)
        requires std::is_move_assignable_v<value_type>
    {
        assert(pos >= cbegin() && pos < cend() && "vector: erase position out of range");

        value_type* slots{store_.data()};
        const size_type at{static_cast<size_type>(pos - cbegin())};
        const size_type last{size_ - 1};

        if (at != last) {
            slots[at] = std::move(slots[last]);
        }

        std::destroy_at(slots + last);
        --size_;

        return slots + at;
    }

    /** @brief Destroy every element, keeping the allocated capacity. */
    void clear() noexcept {
        std::destroy_n(store_.data(), size_);
        size_ = 0;
    }

    /* ========================================================================
     * Observers
     * ======================================================================== */

    /** @brief Access the underlying storage, e.g. to ask a `small_storage` whether it has spilled. */
    constexpr auto& storage(this auto&& self) noexcept { return self.store_; }

private:
    Storage store_{};
    size_type size_{};

    /**
     * @brief Become `other`: steal its block if it has one, otherwise move its elements.
     * @pre This vector holds no live elements.
     */
    void take_over(basic_vector& other) noexcept(std::is_nothrow_move_constructible_v<value_type>) {
        if (store_.adopt_from(other.store_)) {
            size_ = std::exchange(other.size_, 0);
            return;
        }

        /* `other` was still inline, so its elements fit in these inline slots by
         * construction: both arrays are the same length. */
        assert(other.size_ <= store_.capacity() && "vector: unspilled source does not fit the inline slots");

        std::uninitialized_move_n(other.data(), other.size_, store_.data());
        size_ = other.size_;
        other.clear();
    }

    /** @brief Shared body of the two `resize` overloads; `fill` builds the new tail. */
    template <typename F> bool resize_to(const size_type n, F&& fill) {
        if (n <= size_) {
            truncate(n);
            return true;
        }

        if (!reserve(n)) {
            return false;
        }

        /* If this throws it has already destroyed whatever it built, so the
         * vector is left at its original size. */
        std::forward<F>(fill)(store_.data() + size_, n - size_);
        size_ = n;

        return true;
    }
};

/* ============================================================================
 * Aliases
 * ============================================================================ */

/** @brief Growable vector taking every element from a `memory_resource`. */
export template <typename T, memory_resource Resource = default_resource> using vector = basic_vector<dynamic_storage<T, Resource>>;

/**
 * @brief Vector holding its first `N` elements inline and spilling to `Resource` past that.
 *
 * Allocates nothing while `size() <= N`. `storage().spilled()` reports which side
 * of the line it is on.
 */
export template <typename T, std::size_t N, memory_resource Resource = default_resource> using small_vector = basic_vector<small_storage<T, N, Resource>>;

/**
 * @brief Bounded vector holding all `N` elements inline, allocating nothing ever.
 *
 * `push_back` returns `nullptr` once `N` elements are in rather than growing.
 * Neither movable nor copyable, its slots being the object's own bytes.
 */
export template <typename T, std::size_t N> using inline_vector = basic_vector<inline_storage<T, N>>;

/** @brief Bounded vector of `N` elements with the slots on a resource instead of inline. */
export template <typename T, std::size_t N, memory_resource Resource = default_resource> using fixed_vector = basic_vector<fixed_storage<T, N, Resource>>;

/* ============================================================================
 * Concept verification
 * ============================================================================ */

static_assert(std::ranges::contiguous_range<vector<int>>);
static_assert(std::ranges::contiguous_range<const vector<int>>);
static_assert(std::ranges::sized_range<vector<int>>);
static_assert(std::same_as<std::ranges::range_reference_t<vector<int>>, int&>);
static_assert(std::same_as<std::ranges::range_reference_t<const vector<int>>, const int&>);
static_assert(std::constructible_from<vector<int>, std::from_range_t, std::span<const int>>);

/* The heap-backed kinds move by transferring the storage. */
static_assert(vector<int>::growable && vector<int>::relocatable);
static_assert(std::movable<vector<int>>);

/* small_vector moves through adopt_from, not a storage move. */
static_assert(small_vector<int, 8>::growable && small_vector<int, 8>::relocatable);
static_assert(!small_storage<int, 8>::relocatable);
static_assert(std::movable<small_vector<int, 8>>);
static_assert(sizeof(small_vector<int, 8>) >= 8 * sizeof(int));

/* A fixed extent neither grows nor, when it is inline, moves. */
static_assert(!inline_vector<int, 8>::growable && !inline_vector<int, 8>::relocatable);
static_assert(!std::movable<inline_vector<int, 8>>);
static_assert(inline_vector<int, 8>::static_capacity == 8);

/* fixed_vector does not grow either, but its slots are off-object and so do move. */
static_assert(!fixed_vector<int, 8>::growable && fixed_vector<int, 8>::relocatable);
static_assert(std::movable<fixed_vector<int, 8>>);

/* Copying is deleted throughout. */
static_assert(!std::copyable<vector<int>>);

} // namespace libmem
