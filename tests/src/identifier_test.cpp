#include <gtest/gtest.h>

import libmem;
import std;

namespace {

/* The three id shapes the concepts are meant to admit. */

enum class entity : std::uint32_t {
};

struct handle {
    using value_type = std::uint32_t;
    static constexpr value_type null_id{std::numeric_limits<value_type>::max()};

    value_type value{};

    constexpr explicit operator std::uint32_t() const noexcept { return value; }
    constexpr bool operator==(const handle&) const noexcept = default;
};

/* An id that packs a generation into its high bits and masks it off for indexing:
 * the case the to_index hook exists for. */
struct versioned {
    using value_type = std::uint32_t;
    static constexpr value_type null_id{std::numeric_limits<value_type>::max()};

    value_type value{};

    constexpr explicit operator std::uint32_t() const noexcept { return value; }
    constexpr std::size_t to_index() const noexcept { return value & 0xFFFFu; }
    constexpr bool operator==(const versioned&) const noexcept = default;
};

TEST(Identifier, AdmitsUnsignedIntegersEnumsAndStrongIds) {
    static_assert(libmem::regular_indexable_id<std::uint32_t>);
    static_assert(libmem::regular_indexable_id<std::size_t>);
    static_assert(libmem::regular_indexable_id<entity>);
    static_assert(libmem::regular_indexable_id<handle>);
    static_assert(libmem::regular_indexable_id<versioned>);
}

TEST(Identifier, RejectsSignedAndNonIndexableTypes) {
    static_assert(!libmem::explicitly_unsigned<std::int32_t>);
    static_assert(!libmem::indexable_id<std::int32_t>);
    static_assert(!libmem::indexable_id<float>);
    static_assert(!libmem::indexable_id<std::string>);

    /* A signed underlying type disqualifies an enum: it cannot be a subscript. */
    enum class signed_tag : std::int32_t {
    };
    static_assert(!libmem::indexable_enum<signed_tag>);
    static_assert(!libmem::indexable_id<signed_tag>);
}

TEST(Identifier, ScopedEnumsSatisfyValueConstructible) {
    /* Braced, not parenthesised: a scoped enum accepts entity{7u} but not
     * entity(7u), so std::constructible_from would wrongly reject it. */
    static_assert(libmem::value_constructible<entity>);
    static_assert(!std::constructible_from<entity, std::uint32_t>);
    static_assert(libmem::identifier<entity>);
}

TEST(Identifier, ValueTypeResolvesPerShape) {
    static_assert(std::same_as<libmem::id_value_t<std::uint32_t>, std::uint32_t>);
    static_assert(std::same_as<libmem::id_value_t<entity>, std::uint32_t>);
    static_assert(std::same_as<libmem::id_value_t<handle>, std::uint32_t>);
}

TEST(Identifier, NullSentinelIsTheTopOfTheRange) {
    static_assert(libmem::null_id_v<std::uint32_t> == std::numeric_limits<std::uint32_t>::max());
    static_assert(libmem::null_id_v<std::uint8_t> == 255u);
    static_assert(libmem::null_id_v<entity> == entity{std::numeric_limits<std::uint32_t>::max()});

    /* A class id supplies its own. */
    static_assert(libmem::null_id_v<handle> == handle{handle::null_id});
}

TEST(ToIndex, MapsEachShapeToASubscript) {
    EXPECT_EQ(libmem::to_index(std::uint32_t{7}), 7u);
    EXPECT_EQ(libmem::to_index(std::size_t{123456}), 123456u);
    EXPECT_EQ(libmem::to_index(entity{42}), 42u);
    EXPECT_EQ(libmem::to_index(handle{9}), 9u);

    static_assert(libmem::to_index(entity{42}) == 42u);
}

TEST(ToIndex, PrefersAMemberHookOverTheConversionOperator) {
    /* Generation 3 in the high bits, index 5 in the low: the member hook must win,
     * or the sparse array would be sized by the packed value. */
    constexpr versioned id{(3u << 16) | 5u};

    EXPECT_EQ(static_cast<std::uint32_t>(id), (3u << 16) | 5u);
    EXPECT_EQ(libmem::to_index(id), 5u) << "to_index must mask the generation off";
    static_assert(libmem::to_index(id) == 5u);
}

TEST(ToIndex, IsUsableAsAProjection) {
    const std::vector<entity> ids{entity{3}, entity{1}, entity{2}};

    std::vector<std::size_t> indices{};
    std::ranges::transform(ids, std::back_inserter(indices), libmem::to_index);

    EXPECT_EQ(indices, (std::vector<std::size_t>{3, 1, 2}));
    static_assert(libmem::index_mappable<entity>);
}

} // namespace
