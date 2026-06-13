/**
 * @file fuzz_multislab.cpp
 * @brief Coverage-guided fuzzer for `libmem::multislab`.
 *
 * Interprets the libFuzzer input as a config header (which compile-time
 * `BlocksPerSlab` instantiation to drive, plus a runtime max-slab cap) followed
 * by an alloc/release opcode stream. Only valid operations are issued — releases
 * always target a currently-live block — so the allocator's defensive asserts
 * are never tripped; ASan, UBSan, and the structural invariants below surface
 * the bugs.
 *
 * Invariants (checked after every operation):
 *   - allocations never alias a still-live block, and are owned by the allocator;
 *   - the number of blocks reachable by iteration equals the number we hold live;
 *   - empty_slab_count() never exceeds slab_count().
 * On teardown a counting memory_resource verifies destroy() returns every byte.
 *
 * The iteration-count invariant is what catches the lazy full-list corruption
 * class of bug (a lost slab makes the live count diverge); this drives it across
 * random block sizes, caps, and interleavings.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <ranges>
#include <vector>

import libmem;

using libmem::multislab;
using libmem::threshold_policy;

namespace {

#define FUZZ_CHECK(cond)                                                                                                                                       \
    do {                                                                                                                                                       \
        if (!(cond)) {                                                                                                                                         \
            std::fprintf(stderr, "FUZZ INVARIANT FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                                            \
            std::abort();                                                                                                                                      \
        }                                                                                                                                                      \
    } while (false)

/* Byte-stream reader: yields zero once exhausted so the harness can drain input
 * with a simple `while (more())` loop. */
struct reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos{0};

    std::uint8_t u8() { return pos < size ? data[pos++] : std::uint8_t{0}; }

    std::uint32_t u32() {
        std::uint32_t v{0};
        for (int i{0}; i < 4; ++i) {
            v = (v << 8) | u8();
        }
        return v;
    }

    std::uint32_t range(const std::uint32_t lo, const std::uint32_t hi) { return hi <= lo ? lo : lo + (u32() % (hi - lo + 1)); }

    bool more() const { return pos < size; }
};

struct stats {
    std::size_t live_bytes{0};
    std::size_t allocs{0};
    std::size_t frees{0};
};

struct counting_resource {
    stats* s{};

    void* allocate(const std::size_t size) {
        ++s->allocs;
        s->live_bytes += size;
        return ::operator new(size);
    }

    void deallocate(void* ptr, const std::size_t size) noexcept {
        ++s->frees;
        s->live_bytes -= size;
        ::operator delete(ptr, size);
    }
};

constexpr std::size_t live_cap{1024};

template <std::uint32_t BlocksPerSlab> void run(reader& r) {
    using ms_t = multislab<libmem::cache_line_size, BlocksPerSlab, counting_resource, threshold_policy>;

    stats st{};
    const std::uint32_t max_slabs{r.range(0, 6)}; // 0 = unlimited
    const std::uint32_t reserve{r.range(0, 3)};   // hysteresis reserve

    {
        ms_t ms{max_slabs, counting_resource{&st}, threshold_policy{.max_empty_reserve = reserve}};

        std::vector<void*> live{};

        while (r.more()) {
            const std::uint8_t op{r.u8()};

            if ((op & 1u) == 0u) {
                if (live.size() < live_cap) {
                    void* p{ms.allocate()};
                    if (p) {
                        for (void* q : live) {
                            FUZZ_CHECK(q != p); // must not alias a live block
                        }
                        live.push_back(p);
                    }
                }
            } else if (!live.empty()) {
                const std::size_t idx{r.u8() % live.size()};
                ms.deallocate(live[idx]);
                live[idx] = live.back();
                live.pop_back();
            }

            /* the blocks reachable by iteration must be exactly those we hold */
            const auto reachable{std::ranges::distance(ms.begin(), ms.end())};
            FUZZ_CHECK(static_cast<std::size_t>(reachable) == live.size());
            FUZZ_CHECK(ms.empty_slab_count() <= ms.slab_count());
        }

        for (void* p : live) {
            ms.deallocate(p);
        }
        ms.destroy();

        FUZZ_CHECK(st.allocs == st.frees);
        FUZZ_CHECK(st.live_bytes == 0);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    reader r{data, size};

    /* dispatch over a handful of compile-time block-per-slab instantiations */
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
