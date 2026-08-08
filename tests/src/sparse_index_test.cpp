#include <gtest/gtest.h>

import libmem;
import std;

namespace {

enum class entity : std::uint32_t {
};

constexpr std::size_t page_size{libmem::default_sparse_page_size};

/* Counts what reaches the resource, which is how "the paged index did not size
 * itself by the largest id" is actually checked rather than assumed. */
class counting_resource {
public:
    void* allocate(const std::size_t size) {
        ++allocations_;
        bytes_ += size;
        ++live_;
        return ::operator new(size);
    }

    void deallocate(void* ptr, const std::size_t size) noexcept {
        --live_;
        ::operator delete(ptr, size);
    }

    void* allocate(const std::size_t size, const std::size_t align) {
        ++allocations_;
        bytes_ += size;
        ++live_;
        return ::operator new(size, std::align_val_t{align});
    }

    void deallocate(void* ptr, const std::size_t size, const std::size_t align) noexcept {
        --live_;
        ::operator delete(ptr, size, std::align_val_t{align});
    }

    std::size_t allocations() const noexcept { return allocations_; }

    /** @brief Bytes handed out over the resource's whole life, freed or not. */
    std::size_t bytes() const noexcept { return bytes_; }

    int outstanding() const noexcept { return live_; }

private:
    std::size_t allocations_{};
    std::size_t bytes_{};
    int live_{};
};

static_assert(libmem::aligned_memory_resource<counting_resource>);

/* Fails once its budget runs out, to exercise the paths where an index reports
 * that it could not make room. */
class limited_resource {
public:
    constexpr limited_resource() noexcept = default;
    constexpr explicit limited_resource(const int budget) noexcept : budget_{budget} {}

    void* allocate(const std::size_t size) {
        if (budget_ <= 0) {
            return nullptr;
        }
        --budget_;
        return ::operator new(size);
    }

    void deallocate(void* ptr, const std::size_t size) noexcept { ::operator delete(ptr, size); }

    void* allocate(const std::size_t size, const std::size_t align) {
        if (budget_ <= 0) {
            return nullptr;
        }
        --budget_;
        return ::operator new(size, std::align_val_t{align});
    }

    void deallocate(void* ptr, const std::size_t size, const std::size_t align) noexcept { ::operator delete(ptr, size, std::align_val_t{align}); }

private:
    int budget_{};
};

static_assert(libmem::aligned_memory_resource<limited_resource>);

using counted_set = libmem::paged_sparse_set<entity, libmem::resource_ref<counting_resource>>;
using flat_counted_set = libmem::sparse_set<entity, libmem::dynamic_storage<entity, libmem::resource_ref<counting_resource>>>;

/* ============================================================================
 * The index interface
 * ============================================================================ */

TEST(SparseIndexConcepts, BothImplementationsModelTheInterface) {
    static_assert(libmem::sparse_index<libmem::flat_sparse_index<libmem::dynamic_storage<std::size_t>>>);
    static_assert(libmem::sparse_index<libmem::paged_sparse_index<>>);

    /* A flat index over inline storage neither grows nor moves, which is what
     * makes sparse_set over inline_storage a fixed, non-movable set. */
    static_assert(!libmem::flat_sparse_index<libmem::inline_storage<std::size_t, 8>>::growable);
    static_assert(!libmem::flat_sparse_index<libmem::inline_storage<std::size_t, 8>>::relocatable);

    static_assert(libmem::paged_sparse_index<>::growable);
    static_assert(libmem::paged_sparse_index<>::relocatable);

    /* One tombstone value, shared, so the container and its index cannot disagree. */
    static_assert(libmem::sparse_npos == libmem::sparse_set<entity>::npos);
    static_assert(libmem::sparse_npos == libmem::paged_sparse_index<>::npos);
}

TEST(PagedSparseIndex, AnswersForUncoveredSubscriptsWithoutAllocating) {
    counting_resource counter{};
    {
        libmem::paged_sparse_index<page_size, libmem::resource_ref<counting_resource>> index{libmem::resource_ref{counter}};

        EXPECT_EQ(index.covered(), 0u);
        EXPECT_EQ(index.get(0), libmem::sparse_npos);
        EXPECT_EQ(index.get(1'000'000'000), libmem::sparse_npos);
        EXPECT_EQ(counter.allocations(), 0u) << "a probe must never allocate a page";

        ASSERT_TRUE(index.reserve_for(1'000'000));
        index.set(1'000'000, 42);

        EXPECT_EQ(index.get(1'000'000), 42u);
        /* Its neighbours share the page and are tombstoned, not garbage. */
        EXPECT_EQ(index.get(1'000'001), libmem::sparse_npos);
        /* A different page was never allocated, so it answers from the directory. */
        EXPECT_EQ(index.get(2'000'000), libmem::sparse_npos);
        EXPECT_EQ(index.live_page_count(), 1u);
    }
    EXPECT_EQ(counter.outstanding(), 0) << "every page and the directory released";
}

TEST(PagedSparseIndex, HandlesThePageBoundaries) {
    libmem::paged_sparse_index<page_size> index{};

    const std::array<std::size_t, 5> subscripts{0, page_size - 1, page_size, (2 * page_size) - 1, 2 * page_size};

    for (std::size_t i{}; i < subscripts.size(); ++i) {
        ASSERT_TRUE(index.reserve_for(subscripts[i])) << "subscript " << subscripts[i];
        index.set(subscripts[i], i);
    }

    for (std::size_t i{}; i < subscripts.size(); ++i) {
        EXPECT_EQ(index.get(subscripts[i]), i) << "subscript " << subscripts[i];
    }

    EXPECT_EQ(index.live_page_count(), 3u) << "the five subscripts straddle exactly three pages";
    EXPECT_GE(index.covered(), (2 * page_size) + 1);
}

TEST(PagedSparseIndex, AFailedReservationLeavesTheIndexUnchanged) {
    /* One allocation, which the directory takes, leaving nothing for the page. */
    libmem::paged_sparse_index<page_size, limited_resource> index{limited_resource{1}};

    EXPECT_FALSE(index.reserve_for(0));
    EXPECT_EQ(index.get(0), libmem::sparse_npos) << "an unpublished page must not be readable";
    EXPECT_EQ(index.live_page_count(), 0u);

    /* And with no budget at all it is the directory that fails first. */
    libmem::paged_sparse_index<page_size, limited_resource> broke{limited_resource{0}};
    EXPECT_FALSE(broke.reserve_for(0));
    EXPECT_EQ(broke.covered(), 0u);
}

TEST(PagedSparseIndex, MoveTransfersThePages) {
    counting_resource counter{};
    {
        libmem::paged_sparse_index<page_size, libmem::resource_ref<counting_resource>> from{libmem::resource_ref{counter}};

        ASSERT_TRUE(from.reserve_for(5));
        from.set(5, 99);
        const std::size_t allocations{counter.allocations()};

        libmem::paged_sparse_index<page_size, libmem::resource_ref<counting_resource>> to{std::move(from)};

        EXPECT_EQ(to.get(5), 99u);
        EXPECT_EQ(to.live_page_count(), 1u);
        EXPECT_EQ(counter.allocations(), allocations) << "a move allocates nothing";

        EXPECT_EQ(from.covered(), 0u); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        EXPECT_EQ(from.live_page_count(), 0u);
    }
    EXPECT_EQ(counter.outstanding(), 0);
}

/* ============================================================================
 * paged_sparse_set: same behaviour, different sparse side
 * ============================================================================ */

TEST(PagedSparseSet, BehavesExactlyLikeTheFlatOne) {
    libmem::paged_sparse_set<entity> paged{};
    libmem::sparse_set<entity> flat{};

    const std::array ids{entity{3}, entity{700}, entity{1}, entity{99'999}, entity{512}, entity{0}};

    for (const entity id : ids) {
        const auto a{paged.insert(id)};
        const auto b{flat.insert(id)};
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);
        EXPECT_EQ(a.index, b.index);
        EXPECT_EQ(a.inserted, b.inserted);
    }

    EXPECT_EQ(paged.size(), flat.size());
    EXPECT_TRUE(std::ranges::equal(paged, flat));

    /* Re-inserting reports the existing position rather than duplicating. */
    const auto again{paged.insert(entity{700})};
    EXPECT_FALSE(again.inserted);
    EXPECT_EQ(again.index, flat.index_of(entity{700}));

    for (const entity id : ids) {
        EXPECT_TRUE(paged.contains(id));
        EXPECT_EQ(paged.index_of(id), flat.index_of(id));
    }

    EXPECT_FALSE(paged.contains(entity{4}));
    EXPECT_FALSE(paged.contains(entity{100'000'000})) << "far outside every allocated page";
    EXPECT_EQ(paged.index_of(entity{4}), libmem::sparse_set<entity>::npos);
}

TEST(PagedSparseSet, KeepsTheSparseDenseInvariantAcrossInterleavedErases) {
    libmem::paged_sparse_set<entity> paged{};
    std::unordered_set<std::uint32_t> model{};

    /* Ids strided far enough apart that a flat array would be enormous. */
    for (std::uint32_t i{}; i < 200; ++i) {
        const std::uint32_t raw{i * 4093};
        ASSERT_TRUE(paged.insert(entity{raw}));
        model.insert(raw);
    }

    for (std::uint32_t i{}; i < 200; i += 3) {
        const std::uint32_t raw{i * 4093};
        EXPECT_TRUE(paged.erase(entity{raw}));
        model.erase(raw);
    }

    ASSERT_EQ(paged.size(), model.size());
    for (const entity id : paged) {
        EXPECT_TRUE(model.contains(static_cast<std::uint32_t>(id))) << "dense holds an id the model does not";
        EXPECT_EQ(paged[paged.index_of(id)], id) << "sparse and dense disagree for " << static_cast<std::uint32_t>(id);
    }
    for (const std::uint32_t raw : model) {
        EXPECT_TRUE(paged.contains(entity{raw})) << "model holds an id the set lost: " << raw;
    }

    paged.clear();
    EXPECT_TRUE(paged.empty());
    for (const std::uint32_t raw : model) {
        EXPECT_FALSE(paged.contains(entity{raw})) << "clear must restore every tombstone: " << raw;
    }
}

TEST(PagedSparseSet, CostsPagesRatherThanTheWholeIdRange) {
    counting_resource paged_counter{};
    counting_resource flat_counter{};

    /* Twenty ids spread over a 20-million-wide range: the case the flat array is
     * wrong for, and the reason the paged index exists. */
    const auto insert_all = [](auto& set) {
        for (std::uint32_t i{}; i < 20; ++i) {
            ASSERT_TRUE(set.insert(entity{i * 1'000'000}));
        }
    };

    {
        counted_set paged{libmem::resource_ref{paged_counter}};
        flat_counted_set flat{libmem::resource_ref{flat_counter}};

        insert_all(paged);
        insert_all(flat);

        ASSERT_EQ(paged.size(), 20u);
        ASSERT_EQ(flat.size(), 20u);

        /* Twenty ids, twenty distinct pages, because a million apart is far more
         * than one page wide. */
        EXPECT_EQ(paged.sparse().live_page_count(), 20u);

        EXPECT_LT(paged_counter.bytes(), flat_counter.bytes() / 50)
            << "paged: " << paged_counter.bytes() << " bytes, flat: " << flat_counter.bytes() << " bytes";

        /* The flat array really is sized by the largest id; the paged one is not. */
        EXPECT_GE(flat.index_capacity(), 19'000'000u);
        EXPECT_LT(paged.sparse().live_page_count() * page_size, 20'000u);
    }
    EXPECT_EQ(paged_counter.outstanding(), 0);
    EXPECT_EQ(flat_counter.outstanding(), 0);
}

TEST(PagedSparseSet, MovesAndBuildsFromARange) {
    const std::array ids{entity{1}, entity{5000}, entity{7}};

    auto built{ids | std::ranges::to<libmem::paged_sparse_set<entity>>()};
    ASSERT_EQ(built.size(), 3u);
    EXPECT_TRUE(built.contains(entity{5000}));

    libmem::paged_sparse_set<entity> moved{std::move(built)};
    EXPECT_EQ(moved.size(), 3u);
    EXPECT_TRUE(moved.contains(entity{5000}));
    EXPECT_TRUE(moved.contains(entity{7}));
    EXPECT_EQ(built.size(), 0u); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

    libmem::paged_sparse_set<entity> assigned{};
    ASSERT_TRUE(assigned.insert(entity{2}));
    assigned = std::move(moved);
    EXPECT_EQ(assigned.size(), 3u);
    EXPECT_FALSE(assigned.contains(entity{2})) << "the assigned-over contents must be gone";
    EXPECT_TRUE(assigned.contains(entity{1}));
}

TEST(PagedSparseSet, ReportsFailureWhenTheResourceRunsOut) {
    /* One budget shared by both arrays, which is what resource_ref is for: a
     * by-value resource would hand each of them its own. */
    limited_resource pinched{2};
    libmem::paged_sparse_set<entity, libmem::resource_ref<limited_resource>> s{libmem::resource_ref{pinched}};

    /* The first insert needs three blocks: the page directory, the page itself,
     * and the dense array. The dense one is what runs dry here. */
    EXPECT_FALSE(s.insert(entity{1}));
    EXPECT_EQ(s.size(), 0u);
    EXPECT_FALSE(s.contains(entity{1}));

    /* And with only the directory affordable, it is the page that fails. */
    limited_resource tighter{1};
    libmem::paged_sparse_set<entity, libmem::resource_ref<limited_resource>> broke{libmem::resource_ref{tighter}};

    EXPECT_FALSE(broke.insert(entity{1}));
    EXPECT_FALSE(broke.contains(entity{1}));
    EXPECT_EQ(broke.sparse().live_page_count(), 0u) << "a page that could not be allocated must not be published";
}

TEST(PagedSparseSet, SharesOneArenaWithItsDenseArray) {
    libmem::arena scratch{1 << 20};
    const std::size_t before{scratch.used()};

    libmem::paged_sparse_set<entity, libmem::resource_ref<libmem::arena>> s{libmem::resource_ref{scratch}};

    for (std::uint32_t i{}; i < 50; ++i) {
        ASSERT_TRUE(s.insert(entity{i * 700}));
    }

    EXPECT_GT(scratch.used(), before) << "both arrays draw on the injected arena";
    EXPECT_EQ(s.size(), 50u);
    EXPECT_TRUE(s.contains(entity{34'300}));
}

/* ============================================================================
 * paged_sparse_map
 * ============================================================================ */

TEST(PagedSparseMap, KeepsKeysAndPayloadsAlignedAcrossErase) {
    libmem::paged_sparse_map<entity, std::string> m{};

    for (std::uint32_t i{}; i < 30; ++i) {
        const entity id{i * 5000};
        auto [slot, inserted]{m.emplace(id, std::to_string(i))};
        ASSERT_NE(slot, nullptr);
        ASSERT_TRUE(inserted);
    }

    ASSERT_EQ(m.size(), 30u);
    EXPECT_EQ(m.at(entity{25'000}), "5");

    /* Erase from the middle, which swap-and-pops both arrays. */
    EXPECT_TRUE(m.erase(entity{25'000}));
    EXPECT_FALSE(m.contains(entity{25'000}));
    EXPECT_EQ(m.size(), 29u);

    for (const auto& [id, value] : m) {
        const std::uint32_t raw{static_cast<std::uint32_t>(id)};
        EXPECT_EQ(value, std::to_string(raw / 5000)) << "payload drifted away from its key at " << raw;
    }

    m.clear();
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.find(entity{0}), nullptr);
}

TEST(PagedSparseMap, ZipsIntoTheStandardAdaptors) {
    libmem::paged_sparse_map<entity, int> health{};
    ASSERT_NE(health.emplace(entity{9}, 100).first, nullptr);
    ASSERT_NE(health.emplace(entity{90'000}, 50).first, nullptr);

    for (int& hp : health | std::views::values) {
        hp -= 10;
    }

    EXPECT_EQ(health.at(entity{9}), 90);
    EXPECT_EQ(health.at(entity{90'000}), 40);

    auto ids{health | std::views::keys | std::ranges::to<std::vector>()};
    EXPECT_EQ(ids.size(), 2u);
}

/* ============================================================================
 * The flat default is unchanged
 * ============================================================================ */

TEST(FlatSparseIndex, StaysTheDefaultAndKeepsItsFlatCost) {
    libmem::sparse_set<entity> s{};
    ASSERT_TRUE(s.insert(entity{1000}));

    static_assert(std::same_as<libmem::sparse_set<entity>::index_type, libmem::flat_sparse_index<libmem::dynamic_storage<std::size_t>>>);

    EXPECT_GE(s.index_capacity(), 1001u) << "the flat array is still sized by the largest id";
    EXPECT_EQ(s.sparse().covered(), s.index_capacity());
}

} // namespace
