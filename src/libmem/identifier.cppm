/**
 * @file identifier.cppm
 * @brief Concepts and CPOs for types usable as an array index: id traits, null sentinel, `to_index`.
 *
 * An index-addressed container (`sparse_set`, `sparse_map`) needs three things
 * from its key type: a raw value type, a sentinel meaning "no id", and a way to
 * turn an id into a `std::size_t` subscript. Plain `unsigned`, a scoped enum over
 * an unsigned underlying type, and a strong-id class with an unsigned conversion
 * operator all qualify, so a caller does not have to give up type safety to get
 * O(1) lookup.
 *
 * @code
 *     enum class entity : std::uint32_t {};
 *     static_assert(libmem::regular_indexable_id<entity>);
 *
 *     struct handle {
 *         using value_type = std::uint32_t;
 *         static constexpr value_type null_id{0};
 *         value_type v{};
 *         constexpr explicit operator std::uint32_t() const noexcept { return v; }
 *         bool operator==(const handle&) const = default;
 *     };
 *     static_assert(libmem::regular_indexable_id<handle>);
 * @endcode
 */
export module libmem:identifier;

import std;

namespace libmem {

/* ============================================================================
 * Foundation traits
 * ============================================================================ */

/** @brief `T` has a nested `value_type` alias. */
export template <typename T>
concept has_value_type = requires { typename T::value_type; };

/** @brief `T` is hashable via `std::hash`. */
export template <typename T>
concept hashable = requires(const std::decay_t<T>& t) {
    { std::hash<std::decay_t<T>>{}(t) } -> std::convertible_to<std::size_t>;
};

// clang-format off
/** @brief `T` is a class convertible to an unsigned integer via a conversion operator. */
export template <typename T>
concept indexable_class = std::is_class_v<T> && requires(T t) {
    requires
        requires { t.operator std::uint8_t(); } ||
        requires { t.operator std::uint16_t(); } ||
        requires { t.operator std::uint32_t(); } ||
        requires { t.operator std::uint64_t(); } ||
        requires { t.operator std::size_t(); };
};
// clang-format on

/** @brief `T` is a scoped or unscoped enum with an unsigned underlying type. */
export template <typename T>
concept indexable_enum = std::is_enum_v<T> && std::unsigned_integral<std::underlying_type_t<T>>;

/** @brief `T` is unsigned: an unsigned integer, an `indexable_class`, or an `indexable_enum`. */
export template <typename T>
concept explicitly_unsigned = std::unsigned_integral<T> || indexable_class<T> || indexable_enum<T>;

/* ============================================================================
 * Id value type
 * ============================================================================ */

/** @brief Traits class extracting the raw value type of an id `T`. */
export template <typename T> struct id_value;

/** Class with a nested `value_type`. */
template <typename T>
    requires std::is_class_v<T> && has_value_type<T>
struct id_value<T> {
    using type = typename T::value_type;
};

/** Enum: the underlying type. */
template <typename T>
    requires std::is_enum_v<T>
struct id_value<T> {
    using type = std::underlying_type_t<T>;
};

/** Scalar: the decayed type. */
template <typename T>
    requires(!(std::is_class_v<T> || std::is_enum_v<T> || std::is_void_v<T>))
struct id_value<T> {
    using type = std::decay_t<T>;
};

template <typename T>
concept has_id_value = requires { typename id_value<T>::type; };

/** @brief The raw value type of id `T`. */
export template <typename T>
    requires has_id_value<T>
using id_value_t = typename id_value<T>::type;

/** @brief `T` has a resolvable `id_value_t`. */
export template <typename T>
concept value_type_defined = requires { typename id_value_t<T>; };

/**
 * @brief `T` can be built from its own value type.
 *
 * Braced rather than `std::constructible_from`, which is specified in terms of
 * parenthesised direct-initialisation: a scoped enum accepts `E{value}` but not
 * `E(value)` (P0138), so the parenthesised form would reject `enum class entity :
 * std::uint32_t {}` and with it the most idiomatic strong-id shape there is.
 */
export template <typename T>
concept value_constructible = value_type_defined<T> && requires(id_value_t<T> value) { T{value}; };

/* ============================================================================
 * Null-id sentinel
 * ============================================================================ */

namespace detail {

/** @brief Produce a prvalue copy of `value` with decayed type ([expos.only.func]). */
struct decay_copy_fn {
    template <typename T> constexpr std::decay_t<T> operator()(T&& value) const { return std::forward<T>(value); }
};

inline constexpr decay_copy_fn decay_copy{};

template <typename T>
concept has_null_id_member = has_value_type<T> && requires {
    { decay_copy(T::null_id) } -> std::same_as<typename T::value_type>;
};

} // namespace detail

/**
 * @brief Traits class providing the sentinel "no id" value for id `T`.
 *
 * @note For integral and enum ids the sentinel is the maximum representable
 *       value, which therefore is **not** a usable id: the addressable range is
 *       `[0, max())`. A class id opts out by declaring its own static `null_id`.
 */
export template <typename T> struct null_id;

/** Class with its own static `null_id` member. */
template <typename T>
    requires detail::has_null_id_member<T>
struct null_id<T> {
    static constexpr T value{T::null_id};
};

/** Enum: the maximum of the underlying type. */
template <typename T>
    requires indexable_enum<T>
struct null_id<T> {
    static constexpr T value{std::numeric_limits<std::underlying_type_t<T>>::max()};
};

/** Unsigned integral: `numeric_limits::max()`. */
template <typename T>
    requires std::unsigned_integral<T>
struct null_id<T> {
    static constexpr T value{std::numeric_limits<T>::max()};
};

template <typename T>
concept has_null_id_value = requires {
    { detail::decay_copy(null_id<T>::value) } -> std::same_as<std::decay_t<T>>;
};

/**
 * @brief The sentinel "no id" value for id type `T`.
 *
 * A reference to the trait's constant rather than a copy of it: deducing this by
 * value (`constexpr auto`) would copy-construct the sentinel in a constant
 * expression, which demands a `constexpr` copy constructor and so would reject any
 * id whose copy is ordinary runtime code. `null_id<T>::value` is a constexpr static
 * member, so binding to it costs nothing.
 */
export template <typename T>
    requires has_null_id_value<T>
inline constexpr const T& null_id_v{null_id<T>::value};

/** @brief `T` has a well-defined `null_id_v` sentinel. */
export template <typename T>
concept null_identity_defined = requires {
    { detail::decay_copy(null_id_v<T>) } -> std::same_as<std::decay_t<T>>;
};

/* ============================================================================
 * Id concept hierarchy
 * ============================================================================ */

/** @brief Core id concept: resolvable value type, null sentinel, constructible from its value. */
export template <typename T>
concept identifier = value_type_defined<T> && null_identity_defined<T> && value_constructible<T>;

/** @brief `identifier` that is also `std::semiregular`. */
export template <typename T>
concept semiregular_identifier = identifier<T> && std::semiregular<T>;

/** @brief `identifier` that is also `std::regular` (equality-comparable). */
export template <typename T>
concept regular_identifier = identifier<T> && std::regular<T>;

/** @brief `identifier` usable as an array subscript. */
export template <typename T>
concept indexable_id = identifier<T> && explicitly_unsigned<T>;

/** @brief `semiregular_identifier` usable as an array subscript. */
export template <typename T>
concept semiregular_indexable_id = semiregular_identifier<T> && explicitly_unsigned<T>;

/** @brief `regular_identifier` usable as an array subscript: what index-addressed containers need. */
export template <typename T>
concept regular_indexable_id = regular_identifier<T> && explicitly_unsigned<T>;

/** @brief `identifier` that is `hashable`. */
export template <typename T>
concept hashable_id = identifier<T> && hashable<T>;

/* ============================================================================
 * to_index CPO
 * ============================================================================ */

namespace access {

template <typename T>
concept member_to_index = requires(const T& t) {
    { t.to_index() } -> std::convertible_to<std::size_t>;
};

template <typename T>
concept adl_to_index = (std::is_class_v<T> || std::is_enum_v<T>) && requires(const T& t) {
    { to_index(t) } -> std::convertible_to<std::size_t>;
};

/** Poison pill: keeps the unqualified call below from matching unrelated names. */
void to_index() = delete;

/**
 * @brief CPO functor mapping an id to its `std::size_t` subscript.
 *
 * Prefers a member `to_index()`, then ADL `to_index(id)`, then the built-in
 * conversion: `std::to_underlying` for an enum, the unsigned conversion operator
 * for a class, a plain widening for an unsigned integer.
 *
 * A type that packs a generation counter or type tag into its bits will want to
 * mask it off; that is what the member and ADL hooks are for. Without one, the
 * whole value is the index and the container's sparse array is sized by it.
 */
struct to_index_fn {
    template <typename T>
        requires member_to_index<T> || adl_to_index<T> || explicitly_unsigned<T>
    constexpr std::size_t operator()(const T& id) const noexcept {
        if constexpr (member_to_index<T>) {
            return static_cast<std::size_t>(id.to_index());
        } else if constexpr (adl_to_index<T>) {
            return static_cast<std::size_t>(to_index(id));
        } else if constexpr (std::is_enum_v<T>) {
            return static_cast<std::size_t>(std::to_underlying(id));
        } else if constexpr (std::is_class_v<T> && value_type_defined<T>) {
            /* Via the id's own value type, not straight to size_t: an *explicit*
             * conversion function is only a candidate when its return type matches
             * the destination all but exactly ([over.match.conv]), so
             * `static_cast<size_t>` would not even see an
             * `explicit operator std::uint32_t`. */
            return static_cast<std::size_t>(static_cast<id_value_t<T>>(id));
        } else {
            return static_cast<std::size_t>(id);
        }
    }
};

} // namespace access

/** @brief Inline namespace exposing the id CPO instances. */
inline namespace cpo {

/** @brief CPO: maps an id to the `std::size_t` subscript a sparse array indexes by. */
export inline constexpr access::to_index_fn to_index{};

} // namespace cpo

/** @brief `T` can be mapped to a subscript by the `to_index` CPO. */
export template <typename T>
concept index_mappable = std::invocable<const access::to_index_fn&, const T&>;

/* Concept verification over the three id shapes. */
static_assert(regular_indexable_id<std::uint32_t>);
static_assert(null_id_v<std::uint32_t> == std::numeric_limits<std::uint32_t>::max());
static_assert(to_index(std::uint32_t{7}) == 7);

namespace detail {
enum class test_entity : std::uint32_t {
};
} // namespace detail

static_assert(regular_indexable_id<detail::test_entity>);
static_assert(to_index(detail::test_entity{9}) == 9);

} // namespace libmem

namespace libmem::inline cpo {

export using cpo::to_index;

} // namespace libmem::inline cpo
