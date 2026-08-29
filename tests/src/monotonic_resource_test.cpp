/**
 * @file monotonic_resource_test.cpp
 * @brief Tests for `enable_monotonic_resource` and the `monotonic_resource` /
 *        `aligned_monotonic_resource` concepts: which resources classify, how a
 *        reference inherits its referent's category, and caller opt-in.
 */
#include <gtest/gtest.h>

import libmem;
import std;

namespace {

/* A resource that hands out memory and expects it back, the shape the trait exists to exclude. */
struct owning_resource {
    void* allocate(std::size_t size) { return ::operator new(size); }
    void deallocate(void* ptr, std::size_t size) noexcept { ::operator delete(ptr, size); }
    void* allocate(std::size_t size, std::size_t align) { return ::operator new(size, std::align_val_t{align}); }
    void deallocate(void* ptr, std::size_t size, std::size_t align) noexcept { ::operator delete(ptr, size, std::align_val_t{align}); }
};

/* A caller-written bump allocator opting in. */
struct borrowed_bump {
    std::byte* cursor{};
    std::byte* end{};
    void* allocate(std::size_t size) { return allocate(size, alignof(std::max_align_t)); }
    void* allocate(std::size_t size, std::size_t) {
        if (static_cast<std::size_t>(end - cursor) < size) {
            return nullptr;
        }
        return std::exchange(cursor, cursor + size);
    }
    void deallocate(void*, std::size_t) noexcept {}
    void deallocate(void*, std::size_t, std::size_t) noexcept {}
};

} // namespace

template <> inline constexpr bool libmem::enable_monotonic_resource<borrowed_bump> = true;

namespace {

TEST(MonotonicResource, ArenasSatisfyIt) {
    static_assert(libmem::monotonic_resource<libmem::arena>);
    static_assert(libmem::aligned_monotonic_resource<libmem::arena>);
    static_assert(libmem::monotonic_resource<libmem::typed_arena>);
    static_assert(libmem::aligned_monotonic_resource<libmem::typed_arena>);
    SUCCEED();
}

TEST(MonotonicResource, ResourcesExpectingPairedFreesDoNot) {
    static_assert(libmem::aligned_memory_resource<libmem::default_resource>);
    static_assert(!libmem::monotonic_resource<libmem::default_resource>);
    static_assert(!libmem::monotonic_resource<owning_resource>);
    SUCCEED();
}

TEST(MonotonicResource, ReferenceInheritsTheReferentsCategory) {
    static_assert(libmem::monotonic_resource<libmem::resource_ref<libmem::arena>>);
    static_assert(libmem::aligned_monotonic_resource<libmem::resource_ref<libmem::arena>>);
    static_assert(!libmem::monotonic_resource<libmem::resource_ref<libmem::default_resource>>);
    SUCCEED();
}

TEST(MonotonicResource, IsOptInForCallerWrittenResources) {
    static_assert(libmem::monotonic_resource<borrowed_bump>);
    static_assert(libmem::aligned_monotonic_resource<borrowed_bump>);
    SUCCEED();
}

/* An lvalue reference classifies as the bare type; const does not, because
   allocating through a const resource is not possible in the first place. */
TEST(MonotonicResource, LooksThroughReferencesButNotConst) {
    static_assert(libmem::monotonic_resource<libmem::arena&>);
    static_assert(!libmem::memory_resource<const libmem::arena&>);
    static_assert(!libmem::monotonic_resource<const libmem::arena&>);
    SUCCEED();
}

} // namespace
