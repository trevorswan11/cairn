#include <algorithm>
#include <array>
#include <map>
#include <random>

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <stdx/fixed/vector.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/bplus_tree.hh"
#include "storage/error.hh"
#include "storage/page.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::storage;

namespace {

// A deliberately fat key so each node holds only a handful of entries
class fat_key {
  public:
    fat_key() = default;
    template <stdx::NumericIntegral I> explicit fat_key(I v) noexcept : v_{static_cast<i64>(v)} {}

    [[nodiscard]] auto operator<(const fat_key& other) const noexcept -> bool {
        return v_ < other.v_;
    }

  private:
    i64                                    v_;
    [[maybe_unused]] std::array<u8, 1'024> _;
};

} // namespace

TEST_CASE("bplus_tree handles an empty tree") {
    helpers::tempfile file{"bpt_empty"};
    using tree_t = bplus_tree<i64, u64, 64>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};

    CHECK(helpers::unwrap(tree.empty()));
    CHECK_FALSE(helpers::unwrap(tree.contains(42)));
    CHECK(helpers::unwrap_err(tree.get(42)) == error_t::KEY_NOT_FOUND);
}

TEST_CASE("bplus_tree inserts and looks up sequential keys") {
    helpers::tempfile file{"bpt_seq"};
    using tree_t = bplus_tree<i64, u64, 256>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};

    constexpr i64 n{5'000};
    for (i64 i{0}; i < n; ++i) { REQUIRE(tree.emplace(i, static_cast<u64>(i * 10))); }
    CHECK_FALSE(helpers::unwrap(tree.empty()));
    for (i64 i{0}; i < n; ++i) { CHECK(helpers::unwrap(tree.get(i)) == static_cast<u64>(i * 10)); }

    CHECK(helpers::unwrap_err(tree.emplace(123, 0)) == error_t::DUPLICATE_KEY);
    CHECK(helpers::unwrap_err(tree.get(n + 1)) == error_t::KEY_NOT_FOUND);
}

TEST_CASE("bplus_tree matches a std::map oracle under random inserts") {
    helpers::tempfile file{"bpt_rand"};
    using tree_t = bplus_tree<i64, u64, 256>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};

    std::mt19937_64                 rng{Catch::getSeed()};
    stdx::fixed::vector<i64, 4'000> keys;
    for (usize i{0}; i < keys.capacity(); ++i) { keys.emplace_back(i); }
    const auto max{static_cast<i64>(keys.size() - 1)};
    std::ranges::shuffle(keys, rng);

    std::map<i64, u64> oracle;
    for (const i64 k : keys) {
        const auto v{rng()};
        REQUIRE(tree.emplace(k, v));
        oracle.emplace(k, v);
    }
    for (const auto& [k, v] : oracle) { CHECK(helpers::unwrap(tree.get(k)) == v); }

    SECTION("full ascending range scan equals the oracle order") {
        stdx::fixed::vector<std::pair<i64, u64>, keys.capacity()> got;
        const auto                                                count{tree.range_scan(
            0, max, [&](const i64& k, const u64& v) -> void { got.emplace_back(k, v); })};

        CHECK(helpers::unwrap(count) == oracle.size());
        REQUIRE(got.size() == oracle.size());
        CHECK(std::ranges::equal(got, oracle));
    }

    SECTION("bounded range scan returns only keys in [lo, hi]") {
        const auto    inclusive{GENERATE(true, false)};
        constexpr i64 lo{1'000};
        constexpr i64 hi{1'500};

        i64        expected_prev{lo - 1};
        usize      seen{0};
        const auto count{tree.range_scan(
            lo,
            hi,
            [&](const i64& k, const u64&) -> void {
                CHECK(k >= lo);
                CHECK(k <= hi);
                CHECK(k > expected_prev);

                expected_prev = k;
                seen += 1;
            },
            inclusive)};

        CHECK(helpers::unwrap(count) == seen);
        if (inclusive) {
            CHECK(seen == static_cast<usize>(hi - lo + 1));
        } else {
            CHECK(seen == static_cast<usize>(hi - lo));
        }
    }

    SECTION("early-exit scan stops when the visitor returns false") {
        usize      visited{0};
        const auto count{tree.range_scan(
            0, max, [&](const i64&, const u64&) -> bool { return ++visited < 10; })};

        CHECK(visited == 10);
        CHECK(helpers::unwrap(count) == visited);
    }

    SECTION("non-bool visitor values are discarded safely") {
        const auto count{tree.range_scan(
            0, max, [] [[nodiscard]] (const i64&, const u64&) -> double { return 0.0; })};
        CHECK(helpers::unwrap(count) == oracle.size());
    }
}

TEST_CASE("bplus_tree deletes down to empty, matching the oracle") {
    helpers::tempfile file{"bpt_del"};
    using tree_t = bplus_tree<i64, u64, 256>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};

    std::mt19937_64                 rng{Catch::getSeed()};
    stdx::fixed::vector<i64, 3'000> keys;
    for (usize i{0}; i < keys.capacity(); ++i) {
        keys.emplace_back(i);
        REQUIRE(tree.emplace(static_cast<i64>(i), i));
    }
    std::ranges::shuffle(keys, rng);

    std::map<i64, u64> oracle;
    for (usize i{0}; i < keys.capacity(); ++i) { oracle.emplace(static_cast<i64>(i), i); }

    const auto half{keys.size() / 2};
    for (usize i{0}; i < half; ++i) {
        REQUIRE(tree.remove(keys[i]));
        oracle.erase(keys[i]);
    }
    CHECK(helpers::unwrap_err(tree.remove(keys[0])) == error_t::KEY_NOT_FOUND);

    for (const auto& [k, v] : oracle) { CHECK(helpers::unwrap(tree.get(k)) == v); }
    for (usize i{half}; i < keys.size(); ++i) { REQUIRE(tree.remove(keys[i])); }
    CHECK(helpers::unwrap(tree.empty()));
}

TEST_CASE("bplus_tree survives a randomized insert/delete against an oracle") {
    helpers::tempfile file{"bpt_fuzz"};
    using tree_t = bplus_tree<i64, u64, 256>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};

    constexpr i64 lo{0};
    constexpr i64 hi{8'000};

    std::mt19937_64                    rng{Catch::getSeed()};
    std::uniform_int_distribution<i64> key_dist{lo, hi};
    std::bernoulli_distribution        op_dist{0.5};
    std::map<i64, u64>                 oracle;

    for (i32 step{0}; step < 40'000; ++step) {
        const i64 k{key_dist(rng)};
        if (op_dist(rng)) {
            const u64 v{rng()};
            if (oracle.contains(k)) {
                CHECK(helpers::unwrap_err(tree.emplace(k, v)) == error_t::DUPLICATE_KEY);
            } else {
                CHECK(tree.emplace(k, v));
                oracle.emplace(k, v);
            }
        } else {
            if (oracle.contains(k)) {
                CHECK(tree.remove(k));
                oracle.erase(k);
            } else {
                CHECK(helpers::unwrap_err(tree.remove(k)) == error_t::KEY_NOT_FOUND);
            }
        }
    }

    for (const auto& [k, v] : oracle) { CHECK(helpers::unwrap(tree.get(k)) == v); }
    CHECK(helpers::unwrap(tree.range_scan(lo, hi, [](const i64&, const u64&) {})) == oracle.size());
}

TEST_CASE("bplus_tree exercises split/merge/collapse with a small fan-out") {
    using tree_t = bplus_tree<fat_key, u64, 128>;
    STATIC_REQUIRE(tree_t::LEAF_SLOTS < 16);

    helpers::tempfile file{"bpt_wide"};
    auto              pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto              tree{helpers::unwrap(tree_t::create(*pool))};

    std::mt19937_64               rng{Catch::getSeed()};
    stdx::fixed::vector<i64, 800> keys;
    for (usize i{0}; i < keys.capacity(); ++i) { keys.emplace_back(i); }
    std::ranges::shuffle(keys, rng);

    for (const i64 k : keys) { REQUIRE(tree.emplace(fat_key{k}, static_cast<u64>(k))); }
    for (usize i{0}; i < keys.capacity(); ++i) {
        CHECK(helpers::unwrap(tree.get(fat_key{i})) == static_cast<u64>(i));
    }

    std::ranges::shuffle(keys, rng);
    for (const i64 k : keys) { REQUIRE(tree.remove(fat_key{k})); }
    CHECK(helpers::unwrap(tree.empty()));

    REQUIRE(tree.emplace(fat_key{7}, 7));
    CHECK(helpers::unwrap(tree.get(fat_key{7})) == 7);
}

TEST_CASE("bplus_tree persists across a buffer-pool reopen") {
    helpers::tempfile file{"bpt_persist"};
    constexpr i64     n{2'000};
    using tree_t = bplus_tree<i64, u64, 128>;
    page_id_t meta{INVALID_PAGE_ID};

    {
        auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
        auto tree{helpers::unwrap(tree_t::create(*pool))};
        meta = tree.meta_page();

        for (i64 i{0}; i < n; ++i) { REQUIRE(tree.emplace(i, static_cast<u64>(i + 1))); }
        REQUIRE(pool->flush());
    }

    auto   pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    tree_t tree{*pool, meta};
    for (i64 i{0}; i < n; ++i) { CHECK(helpers::unwrap(tree.get(i)) == static_cast<u64>(i + 1)); }
}

} // namespace cairn::tests
