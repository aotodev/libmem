/**
 * @file fuzz_sparse_map.cpp
 * @brief Coverage-guided differential fuzzer for `libmem::sparse_map`.
 *
 * The `sparse_set` harness next door covers the key side; this one exists for the
 * payload. Every operation runs against a `std::unordered_map` model, and the
 * payload type traps lifetime bugs directly: it keeps a live count and a magic
 * word, so a destructor running twice, or a read of a slot whose object is gone,
 * aborts at the point of the mistake rather than showing up as a wrong value later.
 *
 * Invariants (checked after every operation):
 *   - `size()` agrees with the model, and `keys().size() == values().size()`;
 *   - `values()[i]` is the payload the model has for `keys()[i]`, for every slot;
 *   - `find` agrees with the model on presence and on value;
 *   - the number of live payloads equals `size()`.
 *
 * The per-slot alignment check is the load-bearing one. `erase` swaps the last
 * dense key into the hole and mirrors that swap onto the payload array; the two
 * arrays silently drifting apart is the bug class this drives at, and a live count
 * that diverges from `size()` catches the leak and double-destroy variants of the
 * same mistake.
 */
#include "fuzz_support.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>

import libmem;

using libmem::dynamic_storage;
using libmem::fixed_storage;
using libmem::inline_storage;
using libmem::sparse_map;
using libmem::to_index;

namespace {

enum class entity : std::uint32_t {
};

constexpr std::size_t fixed_extent{256};

/**
 * @brief Payload that traps double-destruction and use-after-destroy.
 *
 * `alive` is checked on every access and cleared by the destructor, so a second
 * destructor call or a read of a destroyed slot aborts immediately. `live` is the
 * population count the harness compares against `size()`.
 */
struct guarded {
    static constexpr std::uint32_t magic{0xC0FFEEu};
    static int live;

    std::uint32_t value{};
    std::uint32_t alive{magic};

    explicit guarded(const std::uint32_t v) : value{v} { ++live; }

    guarded(const guarded& other) : value{other.value} {
        FUZZ_CHECK(other.alive == magic);
        ++live;
    }

    guarded(guarded&& other) noexcept : value{other.value} {
        FUZZ_CHECK(other.alive == magic);
        ++live;
    }

    guarded& operator=(const guarded& other) {
        FUZZ_CHECK(alive == magic && other.alive == magic);
        value = other.value;
        return *this;
    }

    guarded& operator=(guarded&& other) noexcept {
        FUZZ_CHECK(alive == magic && other.alive == magic);
        value = other.value;
        return *this;
    }

    ~guarded() {
        FUZZ_CHECK(alive == magic); // a second destructor call lands here
        alive = 0;
        --live;
    }

    std::uint32_t read() const {
        FUZZ_CHECK(alive == magic);
        return value;
    }
};

int guarded::live{0};

/** @brief The model keys on the raw value; `to_index` widens to `size_t`, so narrow back. */
std::uint32_t key_of(const entity id) {
    return static_cast<std::uint32_t>(to_index(id));
}

template <typename Map> void check_invariants(Map& m, const std::unordered_map<std::uint32_t, std::uint32_t>& model) {
    FUZZ_CHECK(m.size() == model.size());
    FUZZ_CHECK(m.empty() == model.empty());

    const auto keys{m.keys()};
    const auto values{m.values()};
    FUZZ_CHECK(keys.size() == values.size());
    FUZZ_CHECK(keys.size() == m.size());

    FUZZ_CHECK(guarded::live == static_cast<int>(model.size()));

    for (std::size_t i{0}; i < keys.size(); ++i) {
        const auto found{model.find(key_of(keys[i]))};
        FUZZ_CHECK(found != model.end());
        FUZZ_CHECK(values[i].read() == found->second); // keys and payloads stay index-aligned
    }

    for (const auto& [v, expected] : model) {
        const auto* payload{m.find(entity{v})};
        FUZZ_CHECK(payload != nullptr);
        FUZZ_CHECK(payload->read() == expected);
    }
}

template <typename Map> void drive(Map& m, fuzz::reader& r, const std::uint32_t id_max) {
    std::unordered_map<std::uint32_t, std::uint32_t> model{};

    while (r.more()) {
        const std::uint8_t op{r.u8()};
        const std::uint32_t v{r.range(0, id_max)};
        const std::uint32_t payload{r.u32()};
        const entity id{v};

        switch (op % 8u) {
        case 0:
        case 1:
        case 2: {
            const auto [slot, inserted]{m.emplace(id, payload)};
            if (inserted) {
                FUZZ_CHECK(slot != nullptr);
                FUZZ_CHECK(slot->read() == payload);
                FUZZ_CHECK(!model.contains(v));
                model.emplace(v, payload);
            } else if (slot != nullptr) {
                /* Already present: emplace must leave the existing payload alone. */
                const auto found{model.find(v)};
                FUZZ_CHECK(found != model.end());
                FUZZ_CHECK(slot->read() == found->second);
            } else {
                FUZZ_CHECK(!model.contains(v)); // storage refused
            }
            break;
        }
        case 3: {
            const auto [slot, inserted]{m.insert_or_assign(id, guarded{payload})};
            if (slot != nullptr) {
                FUZZ_CHECK(slot->read() == payload);
                model[v] = payload;
            } else {
                FUZZ_CHECK(inserted == false);
                FUZZ_CHECK(!model.contains(v));
            }
            break;
        }
        case 4:
        case 5: {
            const bool erased{m.erase(id)};
            FUZZ_CHECK(erased == (model.erase(v) == 1));
            break;
        }
        case 6:
            if ((payload & 1u) == 0u) {
                m.clear();
                model.clear();
            } else {
                static_cast<void>(m.reserve(r.range(0, 512)));
            }
            break;
        default:
            /* Round-trip through a move; the moved-from map must be coherently
             * empty, not one still claiming slots it gave away. */
            if constexpr (Map::relocatable) {
                Map moved{std::move(m)};
                FUZZ_CHECK(m.size() == 0);
                FUZZ_CHECK(m.capacity() == 0);
                m = std::move(moved);
            }
            break;
        }

        check_invariants(m, model);
    }
}

/** @brief Every run must leave no payload alive, whatever the storage kind. */
void check_no_payload_leaked() {
    FUZZ_CHECK(guarded::live == 0);
}

void run_dynamic(fuzz::reader& r, const std::uint32_t id_max) {
    fuzz::stats st{};
    {
        sparse_map<entity, guarded, dynamic_storage<entity, fuzz::counting_resource>> m{fuzz::counting_resource{&st}};
        drive(m, r, id_max);
    }
    fuzz::check_balanced(st);
    check_no_payload_leaked();
}

void run_fixed(fuzz::reader& r, const std::uint32_t id_max) {
    fuzz::stats st{};
    {
        sparse_map<entity, guarded, fixed_storage<entity, fixed_extent, fuzz::counting_resource>> m{fuzz::counting_resource{&st}};
        drive(m, r, id_max);
    }
    fuzz::check_balanced(st);
    check_no_payload_leaked();
}

void run_inline(fuzz::reader& r, const std::uint32_t id_max) {
    {
        sparse_map<entity, guarded, inline_storage<entity, fixed_extent>> m{};
        drive(m, r, id_max);
    }
    check_no_payload_leaked();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::reader r{data, size};

    const std::uint8_t kind{r.u8()};
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
