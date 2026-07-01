#include <algorithm>
#include <random>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/benchmark/catch_chronometer.hpp>
#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/bplus_tree.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::storage;
using tree_t = bplus_tree<i64, u64, 4'096>;

TEST_CASE("bplus_tree throughput", "[.][bench]") {
    helpers::tempfile file{"bpt_bench"};
    auto              pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto              tree{helpers::unwrap(tree_t::create(*pool))};

    constexpr i64    preload{100'000};
    std::mt19937_64  rng{Catch::getSeed()};
    std::vector<i64> keys;
    keys.reserve(static_cast<usize>(preload));
    for (i64 i{0}; i < preload; ++i) { keys.emplace_back(i); }
    std::ranges::shuffle(keys, rng);
    for (const i64 k : keys) { REQUIRE(tree.emplace(k, static_cast<u64>(k))); }
    std::uniform_int_distribution<i64> key_dist{0, preload - 1};

    BENCHMARK("point lookup (hit)") { return tree.get(key_dist(rng)); };

    BENCHMARK("range scan 256 rows") {
        const i64 lo{key_dist(rng)};
        usize     sink{0};
        auto count{tree.range_scan(lo, lo + 255, [&](const i64&, const u64& v) { sink += v; })};
        return sink + (count ? *count : 0);
    };

    BENCHMARK_ADVANCED("bulk insert 10k into a fresh tree")
    (Catch::Benchmark::Chronometer meter) {
        meter.measure([] {
            helpers::tempfile file{"bpt_bench_ins"};
            auto              pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
            auto              tree{helpers::unwrap(tree_t::create(*pool))};
            for (i64 i{0}; i < 10'000; ++i) { DISCARD(tree.emplace(i, static_cast<u64>(i))); }
            return tree.empty();
        });
    };
}

} // namespace cairn::tests
