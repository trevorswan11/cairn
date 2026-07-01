#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdx/fixed/vector.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/bplus_tree.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::storage;
using tree_t = bplus_tree<i64, u64, 1'024>;

TEST_CASE("bplus_tree supports concurrent disjoint inserts") {
    helpers::tempfile file{"bpt_cc_insert"};
    auto              pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto              tree{helpers::unwrap(tree_t::create(*pool))};

    constexpr i64    per_thread{2'000};
    std::atomic<i32> insert_failures{0};

    stdx::fixed::vector<std::thread, 8> workers;
    for (usize t{0}; t < workers.capacity(); ++t) {
        workers.emplace_back([&, t] {
            for (i64 i{0}; i < per_thread; ++i) {
                const i64 key{static_cast<i64>(t) * per_thread + i};
                if (!tree.emplace(key, static_cast<u64>(key))) { insert_failures.fetch_add(1); }
            }
        });
    }
    for (auto& w : workers) { w.join(); }

    CHECK(insert_failures.load() == 0);
    const i64 total{static_cast<i64>(workers.capacity()) * per_thread};
    for (i64 k{0}; k < total; ++k) { CHECK(helpers::unwrap(tree.get(k)) == static_cast<u64>(k)); }

    CHECK(helpers::unwrap(tree.range_scan(0, total - 1, [](const i64&, const u64&) {
              return true;
          })) == static_cast<usize>(total));
}

TEST_CASE("bplus_tree supports concurrent readers, deleters, and inserters") {
    helpers::tempfile file{"bpt_cc_mixed"};
    auto              pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto              tree{helpers::unwrap(tree_t::create(*pool))};

    constexpr i32 writers{4};
    constexpr i64 per_writer{3'000};
    constexpr i64 remove_base{0};
    constexpr i64 stable_base{writers * per_writer}; // [stable_base, stable_base + stable)
    constexpr i64 stable{6'000};
    constexpr i64 emplace_base{stable_base + stable}; // brand-new keys added concurrently

    // Seed the keys the deleters will remove and the readers will probe.
    for (i64 k{remove_base}; k < stable_base + stable; ++k) {
        REQUIRE(tree.emplace(k, static_cast<u64>(k)));
    }

    const auto               seed{Catch::getSeed()};
    std::atomic<i32>         failures{0};
    std::atomic<bool>        go{false};
    std::vector<std::thread> workers;

    // Deleters
    for (i32 t{0}; t < writers; ++t) {
        workers.emplace_back([&, t] {
            while (!go.load());
            for (i64 i{0}; i < per_writer; ++i) {
                const i64 key{static_cast<i64>(t) * per_writer + i};
                if (!tree.remove(key)) { failures.fetch_add(1); }
            }
        });
    }

    // Inserters
    for (i32 t{0}; t < writers; ++t) {
        workers.emplace_back([&, t] {
            while (!go.load());
            for (i64 i{0}; i < per_writer; ++i) {
                const i64 key{emplace_base + static_cast<i64>(t) * per_writer + i};
                if (!tree.emplace(key, static_cast<u64>(key))) { failures.fetch_add(1); }
            }
        });
    }

    // Readers
    for (i32 t{0}; t < writers; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64                    rng{seed + static_cast<u32>(t)};
            std::uniform_int_distribution<i64> dist{stable_base, stable_base + stable - 1};

            while (!go.load());
            for (i32 i{0}; i < 20'000; ++i) {
                const i64 key{dist(rng)};
                if (auto r{tree.get(key)}; !r || *r != static_cast<u64>(key)) {
                    failures.fetch_add(1);
                }
            }
        });
    }

    go.store(true);
    for (auto& w : workers) { w.join(); }
    CHECK(failures.load() == 0);

    // Deleted blocks are gone
    for (i64 k{remove_base}; k < stable_base; ++k) { CHECK_FALSE(tree.get(k)); }

    // Stable region intact
    for (i64 k{stable_base}; k < stable_base + stable; ++k) {
        CHECK(helpers::unwrap(tree.get(k)) == static_cast<u64>(k));
    }

    // Concurrently inserted block present
    const i64 insert_total{static_cast<i64>(writers) * per_writer};
    for (i64 i{0}; i < insert_total; ++i) {
        const i64 k{emplace_base + i};
        CHECK(helpers::unwrap(tree.get(k)) == static_cast<u64>(k));
    }
}

} // namespace cairn::tests
