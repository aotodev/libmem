/**
 * @file fuzz_sparse_set.cpp
 * @brief Coverage-guided differential fuzzer for `libmem::sparse_set`.
 *
 * Reads the libFuzzer input as a config header (which storage kind to instantiate,
 * plus the id range to draw from) followed by an opcode stream, and runs every
 * operation against a `std::unordered_set` model. Only valid operations are issued,
 * so the container's defensive asserts are never tripped; the model comparison and
 * the structural invariants below are what surface bugs.
 *
 * Invariants (checked after every operation):
 *   - `size()` / `empty()` agree with the model, and `keys().size() == size()`;
 *   - every model id is present, and its dense index is in range;
 *   - `index_of(keys()[i]) == i` for every dense slot.
 *
 * That last one is the load-bearing check. Erase is a swap with the last dense
 * element followed by a sparse fix-up, so a sparse array left pointing at the old
 * slot, or an id whose entry was never repointed, breaks the round-trip. It also
 * subsumes a duplicate check: two equal ids could not both round-trip.
 *
 * Storage kinds are all driven, because they differ in whether an insert may be
 * refused: a fixed extent reports failure where a dynamic one grows. The harness
 * mirrors that by updating the model only when the container says it inserted, so
 * a container that wrongly claims success (or wrongly refuses) diverges.
 */
#include "fuzz_support.h"

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>

import libmem;

using libmem::dynamic_storage;
using libmem::fixed_storage;
using libmem::inline_storage;
using libmem::sparse_set;
using libmem::to_index;

namespace {

enum class entity : std::uint32_t {
};

/** @brief Fixed extent used by the non-growable storage kinds. */
constexpr std::size_t fixed_extent{256};

/** @brief The model keys on the raw value; `to_index` widens to `size_t`, so narrow back. */
std::uint32_t key_of(const entity id) {
    return static_cast<std::uint32_t>(to_index(id));
}

template <typename Set> void check_invariants(const Set& s, const std::unordered_set<std::uint32_t>& model) {
    FUZZ_CHECK(s.size() == model.size());
    FUZZ_CHECK(s.empty() == model.empty());
    FUZZ_CHECK(s.keys().size() == s.size());

    for (const std::uint32_t v : model) {
        FUZZ_CHECK(s.contains(entity{v}));
        const auto at{s.index_of(entity{v})};
        FUZZ_CHECK(at < s.size());
        FUZZ_CHECK(key_of(s[at]) == v);
    }

    const auto keys{s.keys()};
    for (std::size_t i{0}; i < keys.size(); ++i) {
        FUZZ_CHECK(model.contains(key_of(keys[i])));
        FUZZ_CHECK(s.index_of(keys[i]) == i); // the sparse/dense round-trip
    }
}

template <typename Set> void drive(Set& s, fuzz::reader& r, const std::uint32_t id_max) {
    std::unordered_set<std::uint32_t> model{};

    while (r.more()) {
        const std::uint8_t op{r.u8()};
        const std::uint32_t v{r.range(0, id_max)};
        const entity id{v};

        switch (op % 8u) {
        /* Weighted towards insertion, so sets actually grow. */
        case 0:
        case 1:
        case 2: {
            const auto result{s.insert(id)};
            if (result.inserted) {
                FUZZ_CHECK(!model.contains(v));
                model.insert(v);
            } else if (result) {
                FUZZ_CHECK(model.contains(v)); // already a member
            } else {
                /* The storage refused; a fixed extent that is full or an id with no
                 * sparse slot. The model must not move. */
                FUZZ_CHECK(!model.contains(v));
            }
            break;
        }
        case 3:
        case 4: {
            const auto removed{s.erase(id)};
            const bool was_member{model.erase(v) == 1};
            FUZZ_CHECK(removed.erased == was_member);
            if (removed.erased) {
                FUZZ_CHECK(removed.index <= s.size());
            }
            break;
        }
        case 5:
            s.clear();
            model.clear();
            break;
        case 6:
            static_cast<void>(s.reserve(r.range(0, 512)));
            static_cast<void>(s.reserve_for(id));
            break;
        default:
            /* Round-trip through a move. The moved-from container must be
             * coherently empty rather than one still claiming slots it gave away. */
            if constexpr (Set::relocatable) {
                Set moved{std::move(s)};
                FUZZ_CHECK(s.size() == 0);
                FUZZ_CHECK(s.capacity() == 0);
                FUZZ_CHECK(s.index_capacity() == 0);
                s = std::move(moved);
            }
            break;
        }

        check_invariants(s, model);
    }
}

void run_dynamic(fuzz::reader& r, const std::uint32_t id_max) {
    fuzz::stats st{};
    {
        sparse_set<entity, dynamic_storage<entity, fuzz::counting_resource>> s{fuzz::counting_resource{&st}};
        drive(s, r, id_max);
    }
    fuzz::check_balanced(st);
}

void run_fixed(fuzz::reader& r, const std::uint32_t id_max) {
    fuzz::stats st{};
    {
        sparse_set<entity, fixed_storage<entity, fixed_extent, fuzz::counting_resource>> s{fuzz::counting_resource{&st}};
        drive(s, r, id_max);
    }
    fuzz::check_balanced(st);
}

void run_inline(fuzz::reader& r, const std::uint32_t id_max) {
    sparse_set<entity, inline_storage<entity, fixed_extent>> s{};
    drive(s, r, id_max);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::reader r{data, size};

    const std::uint8_t kind{r.u8()};

    /* Id range: small ones stress collisions and reuse, the large one stresses
     * sparse growth (the array is sized by the largest id inserted). */
    static constexpr std::uint32_t id_ranges[]{15, 255, 4095, 65535};
    const std::uint32_t id_max{id_ranges[r.u8() % 4u]};

    switch (kind % 3u) {
    case 0:
        run_dynamic(r, id_max);
        break;
    case 1:
        run_fixed(r, id_max);
        break;
    default:
        run_inline(r, id_max);
        break;
    }
    return 0;
}
