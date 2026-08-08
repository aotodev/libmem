/**
 * @file allocator.cppm
 * @brief Standard-Allocator adapter over a libmem `memory_resource`.
 *
 * The mirror of `allocator_resource`, and the two are easy to confuse, so:
 *
 *   - `allocator_resource<Alloc>` is a **resource made from an Allocator**. It lets
 *     a `std::allocator` or `std::pmr::polymorphic_allocator` back a libmem container.
 *   - `resource_allocator<T, R>` is an **Allocator made from a resource**. It lets an
 *     `arena`, a `multislab`, or any other libmem resource back a standard container.
 *
 * @code
 *     libmem::arena scratch{1 << 20};
 *     using alloc = libmem::resource_allocator<int, libmem::resource_ref<libmem::arena>>;
 *
 *     std::vector<int, alloc> v{alloc{libmem::resource_ref{scratch}}};
 *     v.push_back(1);
 * @endcode
 *
 * @warning This is the one place in libmem where allocation failure is reported by
 *          **throwing** `std::bad_alloc`. `Allocator::allocate` is required to
 *          return valid memory or throw, so a null return is not expressible here.
 *          Everything on the libmem side of the boundary still reports by value.
 */
module;

#include <cassert>

export module libmem:allocator;

import :concepts;
import :storage;
import std;

namespace libmem {

/**
 * @brief Standard-conforming Allocator drawing on a libmem `memory_resource`.
 *
 * @tparam T        Element type.
 * @tparam Resource Resource the memory comes from, held by value, exactly as every
 *                  other libmem component holds one. Use `resource_ref<R>` to share
 *                  one resource between several allocators.
 *
 * @note All three `propagate_on_container_*` traits are `true_type`. A container
 *       therefore carries the source's allocator across copy-assign, move-assign,
 *       and swap, which is what keeps those operations well-defined when two
 *       allocators reference different resources; swapping containers with unequal
 *       non-propagating allocators is undefined behaviour.
 *
 * @warning A **stateful** resource must be injected through `resource_ref`, not held
 *          by value. How often a standard container copies its allocator is
 *          unspecified, and a by-value resource is copied with it, so each copy gets
 *          private state. The two implementations really do differ here: a by-value
 *          budget of one serves a thousand `std::vector` reallocations under libc++
 *          and runs out under libstdc++. Stateless resources such as
 *          `default_resource` are unaffected, which is why the default is one.
 */
export template <typename T, memory_resource Resource = default_resource>
    requires std::is_object_v<T>
class resource_allocator {
public:
    using value_type = T;
    using resource_type = Resource;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U> struct rebind {
        using other = resource_allocator<U, Resource>;
    };

    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    constexpr resource_allocator()
        requires std::default_initializable<Resource>
    = default;

    constexpr explicit resource_allocator(Resource resource) noexcept : resource_{std::move(resource)} {}

    /** @brief Rebinding conversion, which is how a node-based container gets its node allocator. */
    template <typename U>
    constexpr resource_allocator(const resource_allocator<U, Resource>& other) noexcept // NOLINT(google-explicit-constructor)
        : resource_{other.resource()} {}

    /**
     * @brief Obtain storage for `n` objects of `T`.
     * @throws std::bad_alloc when the resource could not supply it.
     * @throws std::bad_array_new_length when `n * sizeof(T)` would overflow.
     */
    [[nodiscard]] T* allocate(const size_type n) {
        if (n > std::numeric_limits<size_type>::max() / sizeof(T)) {
            throw std::bad_array_new_length{};
        }

        T* ptr{detail::allocate_slots<T, alignof(T)>(resource_, n)};
        if (!ptr) {
            throw std::bad_alloc{};
        }
        return ptr;
    }

    /**
     * @brief Release storage obtained from `allocate`.
     * @pre `n` is the count `ptr` was allocated with; the resource interface is sized.
     */
    void deallocate(T* ptr, const size_type n) noexcept { detail::free_slots<T, alignof(T)>(resource_, ptr, n); }

    /** @brief Access the backing resource. */
    constexpr auto& resource(this auto&& self) noexcept { return self.resource_; }

    /**
     * @brief Whether memory from one can be released through the other.
     *
     * Compares the resources when they are comparable. When they are not, only a
     * stateless resource can be answered for: an empty type has no state to differ
     * on, so every instance is interchangeable. Anything else answers `false`,
     * which costs a container an element-wise move rather than a pointer steal but
     * is never wrong.
     */
    template <typename U> constexpr bool operator==(const resource_allocator<U, Resource>& other) const noexcept {
        if constexpr (std::equality_comparable<Resource>) {
            return resource_ == other.resource();
        } else {
            return std::is_empty_v<Resource>;
        }
    }

private:
    /* Empty-base-style, so a stateless resource makes an empty allocator and the
     * container that stores one pays nothing for it. */
    [[no_unique_address]] Resource resource_{};
};

/* ============================================================================
 * Concept verification
 * ============================================================================ */

namespace detail {

using arena_alloc = resource_allocator<int, resource_ref<default_resource>>;
using traits = std::allocator_traits<arena_alloc>;

static_assert(std::same_as<traits::value_type, int>);
static_assert(std::same_as<traits::pointer, int*>);
static_assert(std::same_as<traits::rebind_alloc<char>, resource_allocator<char, resource_ref<default_resource>>>);

/* Rebinding must be a conversion, not just a type alias, or a node-based
 * container cannot build its node allocator from the one it was given. */
static_assert(std::constructible_from<traits::rebind_alloc<char>, const arena_alloc&>);

static_assert(std::copyable<arena_alloc>);
static_assert(std::equality_comparable<arena_alloc>);

/* An empty resource makes an empty allocator, so allocator_traits derives
 * is_always_equal for us and a container pays nothing to hold one. */
static_assert(std::is_empty_v<resource_allocator<int, default_resource>>);
static_assert(std::allocator_traits<resource_allocator<int, default_resource>>::is_always_equal::value);
static_assert(!std::allocator_traits<arena_alloc>::is_always_equal::value);

} // namespace detail

} // namespace libmem
