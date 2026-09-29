// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file fuzz_support.h
 * @brief Shared plumbing for the libFuzzer harnesses.
 *
 * The input reader, the invariant-check macro, a byte-counting
 * `memory_resource` used by every target to prove teardown returns everything it
 * took, and the harnesses' record of what is live.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <unordered_map>
#include <vector>

/** @brief Abort with the failing expression; libFuzzer records the input. */
#define FUZZ_CHECK(cond)                                                                                                                                       \
    do {                                                                                                                                                       \
        if (!(cond)) {                                                                                                                                         \
            std::fprintf(stderr, "FUZZ INVARIANT FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                                            \
            std::abort();                                                                                                                                      \
        }                                                                                                                                                      \
    } while (false)

namespace fuzz {

/**
 * @brief Byte-stream reader over the libFuzzer input.
 *
 * Yields zero once exhausted, so a harness can drain the input with a plain
 * `while (more())` loop and never has to bounds-check.
 */
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

/**
 * @brief Resource tallying bytes in and out; held by pointer so the harness can
 *        still read the totals after the container that owned a copy is gone.
 */
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

    /* multislab takes its slab memory through these, so a resource backing one must be an
     * aligned_memory_resource. */
    void* allocate(const std::size_t size, const std::size_t align) {
        ++s->allocs;
        s->live_bytes += size;
        return ::operator new(size, std::align_val_t{align});
    }

    void deallocate(void* ptr, const std::size_t size, const std::size_t align) noexcept {
        ++s->frees;
        s->live_bytes -= size;
        ::operator delete(ptr, size, std::align_val_t{align});
    }
};

/** @brief The harness's record of live keys: O(1) insert, erase, lookup and uniform pick. */
template <typename K> class live_set {
public:
    bool contains(const K& key) const { return index_.contains(key); }
    std::size_t size() const { return keys_.size(); }
    bool empty() const { return keys_.empty(); }
    const std::vector<K>& keys() const { return keys_; }

    /** @return `false` when `key` is already live. */
    bool insert(const K& key) {
        if (!index_.emplace(key, keys_.size()).second) {
            return false;
        }
        keys_.push_back(key);
        return true;
    }

    void erase(const K& key) {
        const auto it{index_.find(key)};
        FUZZ_CHECK(it != index_.end());
        const std::size_t i{it->second};
        index_.erase(it);
        if (i + 1 != keys_.size()) {
            keys_[i] = keys_.back();
            index_[keys_[i]] = i;
        }
        keys_.pop_back();
    }

    /** @pre `!empty()`. */
    const K& pick(reader& r) const { return keys_[r.range(0, static_cast<std::uint32_t>(keys_.size() - 1))]; }

private:
    std::vector<K> keys_{};
    std::unordered_map<K, std::size_t> index_{};
};

/** @brief Assert the resource handed back every byte it gave out. */
inline void check_balanced(const stats& s) {
    FUZZ_CHECK(s.allocs == s.frees);
    FUZZ_CHECK(s.live_bytes == 0);
}

} // namespace fuzz
