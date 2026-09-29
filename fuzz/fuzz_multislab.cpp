// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file fuzz_multislab.cpp
 * @brief Coverage-guided fuzzer for `libmem::multislab`.
 *
 * A config header picks a compile-time `BlocksPerSlab`, a runtime slab cap and a
 * hysteresis reserve; the rest is an opcode stream of allocations, releases (by
 * pointer and by iterator), and traversals that mutate the allocator as they go.
 * Only valid operations are issued, so the allocator's asserts never fire.
 *
 * Invariants:
 *   - an allocation succeeds iff the cap leaves room, and never aliases a live block;
 *   - `begin()` reaches exactly the live blocks; `empty_slab_count() <= slab_count()`;
 *   - a traversal that releases and allocates while it walks visits only live blocks,
 *     none twice, and every block live for the whole walk exactly once;
 *   - an `allocate_at` iterator held across operations still starts at its block;
 *   - teardown returns every byte to the resource.
 */
#include "fuzz_support.h"

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <unordered_set>

import libmem;

using libmem::multislab;
using libmem::threshold_policy;

namespace {

constexpr std::size_t live_cap{1024};

template <std::uint32_t BlocksPerSlab> class harness {
    using multislab_type = multislab<libmem::cache_line_size, BlocksPerSlab, fuzz::counting_resource, threshold_policy>;

public:
    harness(fuzz::reader& r, fuzz::stats& st, const std::uint32_t max_slabs, const std::uint32_t reserve)
        : r_{r}, max_slabs_{max_slabs}, ms_{max_slabs, fuzz::counting_resource{&st}, threshold_policy{.max_empty_reserve = reserve}} {}

    void run() {
        while (r_.more()) {
            switch (r_.u8() % 6u) {
            case 0:
            case 1:
                allocate();
                break;
            case 2:
            case 3:
                release_any(nullptr);
                break;
            case 4:
                walk();
                break;
            default:
                hold();
                break;
            }
            check();
        }

        for (void* p : live_.keys()) {
            ms_.deallocate(p);
        }
        ms_.destroy();
    }

private:
    fuzz::reader& r_;
    std::uint32_t max_slabs_;
    multislab_type ms_;
    fuzz::live_set<void*> live_{};
    /* Everything released during the current walk; a reused address is not "live throughout". */
    std::unordered_set<void*> released_{};

    bool has_room() const { return max_slabs_ == 0 || live_.size() < std::size_t{max_slabs_} * BlocksPerSlab; }

    void allocate() {
        if (live_.size() >= live_cap) {
            return;
        }
        const bool room{has_room()};
        void* p{ms_.allocate()};
        FUZZ_CHECK((p != nullptr) == room);
        if (p) {
            FUZZ_CHECK(live_.insert(p)); // aliasing a live block fails the insert
        }
    }

    void release(void* p) {
        if (r_.u8() & 1u) {
            ms_.deallocate(ms_.make_iterator(p));
        } else {
            ms_.deallocate(p);
        }
        live_.erase(p);
        released_.insert(p);
    }

    /** @brief Release a random live block other than `keep`. */
    void release_any(const void* keep) {
        if (live_.empty()) {
            return;
        }
        void* p{live_.pick(r_)};
        if (p != keep) {
            release(p);
        }
    }

    /* Erase-while-iterating, plus allocations and releases elsewhere, one per step. */
    void walk() {
        const auto before{live_.keys()};
        std::unordered_set<void*> seen{};
        released_.clear();

        for (auto it{ms_.begin()}; it != ms_.end();) {
            void* const p{*it};
            FUZZ_CHECK(live_.contains(p));
            FUZZ_CHECK(seen.insert(p).second);
            ++it;
            switch (r_.u8() % 4u) {
            case 0:
                release(p);
                break;
            case 1:
                allocate();
                break;
            case 2:
                release_any(it == ms_.end() ? nullptr : *it);
                break;
            default:
                break;
            }
        }

        for (void* p : before) {
            FUZZ_CHECK(released_.contains(p) || seen.contains(p));
        }
    }

    void hold() {
        if (live_.size() >= live_cap) {
            return;
        }
        const bool room{has_room()};
        const auto held{ms_.allocate_at()};
        FUZZ_CHECK((held.ptr != nullptr) == room);
        if (!held.ptr) {
            return;
        }
        FUZZ_CHECK(live_.insert(held.ptr));

        for (std::uint32_t n{r_.range(1, 8)}; n > 0 && r_.more(); --n) {
            if (r_.u8() & 1u) {
                allocate();
            } else {
                release_any(held.ptr);
            }
        }

        auto it{held.it};
        FUZZ_CHECK(*it == held.ptr);
        std::unordered_set<void*> seen{};
        for (; it != ms_.end(); ++it) {
            FUZZ_CHECK(live_.contains(*it));
            FUZZ_CHECK(seen.insert(*it).second);
        }
    }

    void check() {
        FUZZ_CHECK(static_cast<std::size_t>(std::ranges::distance(ms_.begin(), ms_.end())) == live_.size());
        FUZZ_CHECK(ms_.empty_slab_count() <= ms_.slab_count());
        FUZZ_CHECK(max_slabs_ == 0 || ms_.slab_count() <= max_slabs_);
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
    fuzz::check_balanced(st);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::reader r{data, size};

    switch (r.u8() % 5u) {
    case 0:
        run<1>(r);
        break;
    case 1:
        run<2>(r);
        break;
    case 2:
        run<4>(r);
        break;
    case 3:
        run<16>(r);
        break;
    default:
        run<64>(r);
        break;
    }
    return 0;
}
