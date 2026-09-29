// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file fuzz_pool.cpp
 * @brief Coverage-guided fuzzer for `libmem::pool`.
 *
 * A config header picks a compile-time `BlocksPerSlab`, a slab cap and a hysteresis
 * reserve; the rest is an opcode stream of inserts, erases through stored iterators,
 * erase-while-iterating sweeps, clones and clears. Every element carries a unique id
 * and a magic word its destructor clears, so a read of a destroyed element or a
 * second destructor call aborts where it happens.
 *
 * Invariants (after every operation):
 *   - an insert succeeds iff the cap leaves room;
 *   - `size()`, the live element count and the length of `begin()..end()` agree with the model;
 *   - every stored iterator still dereferences to its own element;
 *   - `erase` returns `end()` or a live element;
 *   - a sweep visits only live elements, none twice, and every element live for the
 *     whole sweep exactly once;
 *   - `try_clone` holds the same ids;
 *   - teardown destroys every element and returns every byte to the resource.
 */
#include "fuzz_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <vector>

import libmem;

using libmem::threshold_policy;

namespace {

constexpr std::size_t live_cap{1024};

struct guarded {
    static constexpr std::uint32_t magic{0xC0FFEEu};
    static int live;

    std::uint32_t id{};
    std::uint32_t alive{magic};

    explicit guarded(const std::uint32_t v) : id{v} { ++live; }

    guarded(const guarded& other) : id{other.read()} { ++live; }
    guarded(guarded&& other) noexcept : id{other.read()} { ++live; }
    guarded& operator=(const guarded&) = delete;
    guarded& operator=(guarded&&) = delete;

    ~guarded() {
        FUZZ_CHECK(alive == magic); // a second destructor call lands here
        alive = 0;
        --live;
    }

    std::uint32_t read() const {
        FUZZ_CHECK(alive == magic);
        return id;
    }
};

int guarded::live{0};

template <std::uint32_t BlocksPerSlab> class harness {
    using pool_type = libmem::pool<guarded, BlocksPerSlab, fuzz::counting_resource, threshold_policy>;
    using iterator = typename pool_type::iterator;

public:
    harness(fuzz::reader& r, fuzz::stats& st, const std::uint32_t max_slabs, const std::uint32_t reserve)
        : r_{r}, max_slabs_{max_slabs}, pool_{libmem::slab_limit{max_slabs}, fuzz::counting_resource{&st}, threshold_policy{.max_empty_reserve = reserve}} {}

    void run() {
        while (r_.more()) {
            switch (r_.u8() % 16u) {
            case 0:
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
                insert();
                break;
            case 6:
            case 7:
            case 8:
            case 9:
                erase_any();
                break;
            case 10:
            case 11:
                sweep();
                break;
            case 12:
                clone();
                break;
            case 13:
                pool_.clear();
                ids_ = {};
                where_.clear();
                break;
            default:
                break;
            }
            check();
        }
    }

private:
    fuzz::reader& r_;
    std::uint32_t max_slabs_;
    pool_type pool_;
    fuzz::live_set<std::uint32_t> ids_{};
    std::unordered_map<std::uint32_t, iterator> where_{};
    std::uint32_t next_id_{0};

    bool has_room() const { return max_slabs_ == 0 || ids_.size() < std::size_t{max_slabs_} * BlocksPerSlab; }

    void insert() {
        if (ids_.size() >= live_cap) {
            return;
        }
        const bool room{has_room()};
        const std::uint32_t id{next_id_++};
        const auto it{pool_.emplace(id)};
        FUZZ_CHECK((it != pool_.end()) == room);
        if (it != pool_.end()) {
            FUZZ_CHECK(it->read() == id);
            FUZZ_CHECK(ids_.insert(id));
            where_.emplace(id, it);
        }
    }

    void forget(const std::uint32_t id) {
        ids_.erase(id);
        where_.erase(id);
    }

    void check_next(const iterator next) const { FUZZ_CHECK(next == pool_.end() || ids_.contains(next->read())); }

    void erase_any() {
        if (ids_.empty()) {
            return;
        }
        const std::uint32_t id{ids_.pick(r_)};
        const iterator it{where_.at(id)};
        FUZZ_CHECK(it->read() == id);
        const auto next{pool_.erase(it)};
        forget(id);
        check_next(next);
    }

    /* The erase-while-iterating idiom, with inserts along the way. Ids are never
     * reused, so an id still live at the end was live for the whole sweep. */
    void sweep() {
        const auto before{ids_.keys()};
        std::unordered_set<std::uint32_t> seen{};

        for (auto it{pool_.begin()}; it != pool_.end();) {
            const std::uint32_t id{it->read()};
            FUZZ_CHECK(ids_.contains(id));
            FUZZ_CHECK(seen.insert(id).second);
            switch (r_.u8() % 3u) {
            case 0:
                it = pool_.erase(it);
                forget(id);
                check_next(it);
                break;
            case 1:
                ++it;
                insert();
                break;
            default:
                ++it;
                break;
            }
        }

        for (const std::uint32_t id : before) {
            FUZZ_CHECK(!ids_.contains(id) || seen.contains(id));
        }
    }

    void clone() {
        auto copy{pool_.try_clone()};
        FUZZ_CHECK(copy.has_value()); // a dense copy never needs more slabs than the original
        FUZZ_CHECK(copy->size() == pool_.size());
        FUZZ_CHECK(guarded::live == static_cast<int>(2 * ids_.size()));

        auto ids{*copy | std::views::transform(&guarded::read) | std::ranges::to<std::vector>()};
        auto expected{ids_.keys()};
        std::ranges::sort(ids);
        std::ranges::sort(expected);
        FUZZ_CHECK(ids == expected);
    }

    void check() {
        FUZZ_CHECK(pool_.size() == ids_.size());
        FUZZ_CHECK(guarded::live == static_cast<int>(ids_.size()));
        FUZZ_CHECK(static_cast<std::size_t>(std::ranges::distance(pool_.begin(), pool_.end())) == ids_.size());
        FUZZ_CHECK(max_slabs_ == 0 || pool_.slab_count() <= max_slabs_);
        if (!ids_.empty()) {
            const std::uint32_t id{ids_.pick(r_)};
            FUZZ_CHECK(where_.at(id)->read() == id);
        }
    }
};

template <std::uint32_t BlocksPerSlab> void run(fuzz::reader& r) {
    fuzz::stats st{};
    const std::uint32_t max_slabs{r.range(0, 6)}; // 0 = unlimited
    const std::uint32_t reserve{r.range(0, 3)};
    {
        harness<BlocksPerSlab> h{r, st, max_slabs, reserve};
        h.run();
    }
    FUZZ_CHECK(guarded::live == 0);
    fuzz::check_balanced(st);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::reader r{data, size};

    switch (r.u8() % 3u) {
    case 0:
        run<1>(r);
        break;
    case 1:
        run<4>(r);
        break;
    default:
        run<64>(r);
        break;
    }
    return 0;
}
