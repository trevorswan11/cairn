#include <atomic>
#include <chrono>
#include <cstddef>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "exec/table_scan.hh"
#include "support/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/mt_verifier.hh"
#include "testhelpers/scheduler.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/lock/manager.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::txn;
using namespace stdx::size_literals;
using namespace helpers::common_tids;
using helpers::deterministic_scheduler;

using txn_tree_t   = iot_tree<i64, 128, 64>;
using table_scan_t = exec::table_scan<i64, 128, 64>;

TEST_CASE("concurrency dirty read prevention") {
    helpers::tempfile      file{"txn_concurrency_dirty_read_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    helpers::tempfile      wal_file{"txn_concurrency_dirty_read_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    // Initialize key 1 with value 100
    {
        const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string("100")));
        REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
        REQUIRE(tm.commit_txn(init_txn, lm));
    }

    helpers::mt_verifier    verifier;
    deterministic_scheduler scheduler{{tid0, tid1, tid0, tid1, tid0, tid1}};

    {
        // Thread 0: T1 updates key 1 to 200, then aborts
        std::jthread t0{[&] {
            const auto t1{tm.begin_txn(isolation_level_t::SNAPSHOT)};
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // Update key 1 to 200
            MT_REQUIRE(verifier, tree.update_txn(t1, 1, helpers::span_from_string("200")));
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // Wait for T2 to read
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // Abort T1
            MT_REQUIRE(verifier, tm.update_txn_lsn(t1, wal::log::seq_num{2}));
            MT_REQUIRE(verifier, tree.rollback_txn(t1));
            MT_REQUIRE(verifier, tm.abort_txn(t1, lm));
            scheduler.thread_exit(tid0);
        }};

        // Thread 1: T2 reads key 1 (should see 100, not 200)
        std::jthread t1{[&] {
            const auto t2{tm.begin_txn(isolation_level_t::READ_COMMITTED)};
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));

            // Let T1 do the update
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));

            // T2 reads key 1: must see 100 (uncommitted 200 must be invisible)
            const auto             snap2{MT_UNWRAP(verifier, tm.acquire_snapshot(t2))};
            std::vector<std::byte> buf;
            const auto             val{MT_UNWRAP(verifier, tree.get_txn(t2, snap2, tm, 1, buf))};
            const auto             span{MT_UNWRAP(verifier, val)};
            MT_CHECK(verifier, helpers::string_from_span(span) == "100");

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));

            // Commit T2
            MT_REQUIRE(verifier, tm.update_txn_lsn(t2, wal::log::seq_num{3}));
            MT_CHECK(verifier, tm.commit_txn(t2, lm).has_value());
            scheduler.thread_exit(tid1);
        }};
    }

    REQUIRE_FALSE(verifier.dump_if_error());
}

TEST_CASE("concurrency non-repeatable read behavior") {
    helpers::tempfile      file{"txn_concurrency_non_repeatable_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    helpers::tempfile      wal_file{"txn_concurrency_non_repeatable_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    // Initialize key 1 with value 100
    {
        const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string("100")));
        REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
        REQUIRE(tm.commit_txn(init_txn, lm));
    }

    helpers::mt_verifier verifier;

    // Scenario 1: READ_COMMITTED should allow non-repeatable reads
    {
        deterministic_scheduler scheduler{{tid0, tid1, tid0, tid1, tid0, tid1}};

        std::jthread t0{[&] {
            const auto t1{tm.begin_txn(isolation_level_t::READ_COMMITTED)};
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // First read: sees 100
            const auto             snap1{MT_UNWRAP(verifier, tm.acquire_snapshot(t1))};
            std::vector<std::byte> buf;
            const auto             val1{MT_UNWRAP(verifier, tree.get_txn(t1, snap1, tm, 1, buf))};
            const auto             span1{MT_UNWRAP(verifier, val1)};
            MT_CHECK(verifier, helpers::string_from_span(span1) == "100");

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // Second read: sees 200
            const auto val2{MT_UNWRAP(verifier, tree.get_txn(t1, snap1, tm, 1, buf))};
            const auto span2{MT_UNWRAP(verifier, val2)};
            MT_CHECK(verifier, helpers::string_from_span(span2) == "200");

            MT_CHECK(verifier, tm.commit_txn(t1, lm).has_value());
            scheduler.thread_exit(tid0);
        }};

        std::jthread t1{[&] {
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1)); // wait for T1's first read

            // T2 updates key 1 to 200 and commits
            const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};
            MT_REQUIRE(verifier, tree.update_txn(t2, 1, helpers::span_from_string("200")));
            MT_REQUIRE(verifier, tm.update_txn_lsn(t2, wal::log::seq_num{2}));
            MT_CHECK(verifier, tm.commit_txn(t2, lm).has_value());

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));
            scheduler.thread_exit(tid1);
        }};
    }

    // Scenario 2: REPEATABLE_READ should prevent non-repeatable reads
    {
        deterministic_scheduler scheduler{{tid0, tid1, tid0, tid1, tid0, tid1}};

        std::jthread t0{[&] {
            const auto t1{tm.begin_txn(isolation_level_t::REPEATABLE_READ)};
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // First read: sees 200 (since initial state was updated to 200 in Scenario 1)
            const auto             snap1{MT_UNWRAP(verifier, tm.acquire_snapshot(t1))};
            std::vector<std::byte> buf;
            const auto             val1{MT_UNWRAP(verifier, tree.get_txn(t1, snap1, tm, 1, buf))};
            const auto             span1{MT_UNWRAP(verifier, val1)};
            MT_CHECK(verifier, helpers::string_from_span(span1) == "200");

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // Second read: must still see 200 (not 300)
            const auto val2{MT_UNWRAP(verifier, tree.get_txn(t1, snap1, tm, 1, buf))};
            const auto span2{MT_UNWRAP(verifier, val2)};
            MT_CHECK(verifier, helpers::string_from_span(span2) == "200");

            MT_CHECK(verifier, tm.commit_txn(t1, lm).has_value());
            scheduler.thread_exit(tid0);
        }};

        std::jthread t1{[&] {
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1)); // wait for T1's first read

            // T2 updates key 1 to 300 and commits
            const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};
            MT_REQUIRE(verifier, tree.update_txn(t2, 1, helpers::span_from_string("300")));
            MT_REQUIRE(verifier, tm.update_txn_lsn(t2, wal::log::seq_num{3}));
            MT_CHECK(verifier, tm.commit_txn(t2, lm).has_value());

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));
            scheduler.thread_exit(tid1);
        }};
    }

    REQUIRE_FALSE(verifier.dump_if_error());
}

TEST_CASE("concurrency lost update behavior") {
    helpers::tempfile      file{"txn_concurrency_lost_update_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    helpers::tempfile      wal_file{"txn_concurrency_lost_update_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    // Initialize key 1 with value 100
    {
        const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string("100")));
        REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
        REQUIRE(tm.commit_txn(init_txn, lm));
    }

    helpers::mt_verifier verifier;

    // Under SERIALIZABLE, lost updates are prevented via serialization failure
    {
        deterministic_scheduler scheduler{{tid0, tid1, tid0, tid1, tid0, tid1, tid0, tid1}};

        std::jthread t0{[&] {
            const auto t1{tm.begin_txn(isolation_level_t::SERIALIZABLE)};
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            const auto             snap1{MT_UNWRAP(verifier, tm.acquire_snapshot(t1))};
            std::vector<std::byte> buf;
            const auto             val1{MT_UNWRAP(verifier, tree.get_txn(t1, snap1, tm, 1, buf))};
            DISCARD(MT_UNWRAP(verifier, val1));

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0)); // wait for T2 to read

            MT_REQUIRE(verifier, tree.update_txn(t1, 1, helpers::span_from_string("150")));
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0)); // wait for T2 to write

            // Commit T1: should succeed
            MT_REQUIRE(verifier, tm.update_txn_lsn(t1, wal::log::seq_num{2}));
            MT_CHECK(verifier, tm.commit_txn(t1, lm).has_value());

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));
            scheduler.thread_exit(tid0);
        }};

        std::jthread t1{[&] {
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1)); // wait for T1 to read

            const auto             t2{tm.begin_txn(isolation_level_t::SERIALIZABLE)};
            const auto             snap2{MT_UNWRAP(verifier, tm.acquire_snapshot(t2))};
            std::vector<std::byte> buf{};
            const auto             val2{MT_UNWRAP(verifier, tree.get_txn(t2, snap2, tm, 1, buf))};
            DISCARD(MT_UNWRAP(verifier, val2));

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1)); // wait for T1 to write

            MT_REQUIRE(verifier, tree.update_txn(t2, 1, helpers::span_from_string("250")));
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1)); // wait for T1 to commit

            // Commit T2: must fail with serialization failure
            MT_REQUIRE(verifier, tm.update_txn_lsn(t2, wal::log::seq_num{3}));
            MT_CHECK(verifier,
                     MT_UNWRAP_ERR(verifier, tm.commit_txn(t2, lm)) ==
                         error_t::TXN_SERIALIZATION_FAILURE);
            MT_REQUIRE(verifier, tree.rollback_txn(t2));

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));
            scheduler.thread_exit(tid1);
        }};
    }

    REQUIRE_FALSE(verifier.dump_if_error());
}

TEST_CASE("concurrency phantom read behavior") {
    helpers::tempfile      file{"txn_concurrency_phantom_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm{};
    helpers::tempfile      wal_file{"txn_concurrency_phantom_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    helpers::mt_verifier verifier{};

    // Scenario 1: READ_COMMITTED allows phantoms
    {
        deterministic_scheduler scheduler{{tid0, tid1, tid0, tid1, tid0, tid1}};

        std::jthread t0{[&] {
            const auto t1{tm.begin_txn(isolation_level_t::READ_COMMITTED)};
            const auto snap1{MT_UNWRAP(verifier, tm.acquire_snapshot(t1))};

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // First scan: sees 0 keys in range [10, 20]
            table_scan_t scanner{tree, t1, snap1, tm};
            usize        count1{0};
            MT_REQUIRE(verifier, scanner(10, 20, [&](const auto&, auto) { count1++; }));
            MT_CHECK(verifier, count1 == 0);

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // Second scan: should see 1 key (phantom read)
            table_scan_t scanner2{tree, t1, snap1, tm};
            usize        count2{0};
            MT_REQUIRE(verifier, scanner2(10, 20, [&](const auto&, auto) { count2++; }));
            MT_CHECK(verifier, count2 == 1);

            MT_CHECK(verifier, tm.commit_txn(t1, lm).has_value());
            scheduler.thread_exit(tid0);
        }};

        std::jthread t1{[&] {
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1)); // wait for T1 first scan

            const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};
            MT_REQUIRE(verifier, tree.insert_txn(t2, 15, helpers::span_from_string("val15")));
            MT_REQUIRE(verifier, tm.update_txn_lsn(t2, wal::log::seq_num{1}));
            MT_CHECK(verifier, tm.commit_txn(t2, lm).has_value());

            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));
            MT_REQUIRE(verifier, scheduler.yield_to_schedule(tid1)); // wait for T1 second scan
            scheduler.thread_exit(tid1);
        }};
    }

    REQUIRE_FALSE(verifier.dump_if_error());
}

TEST_CASE("concurrency stress bank transfers") {
    helpers::tempfile      file{"txn_concurrency_stress_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm{};
    lock::manager          lm_lock{};
    tm.set_lock_manager(lm_lock);
    helpers::tempfile wal_file{"txn_concurrency_stress_test_wal"};
    wal::log::manager lm{wal_file.path, 1_MiB};

    constexpr i64 num_accounts{10};
    constexpr i64 initial_balance{1'000};
    constexpr i64 total_money{num_accounts * initial_balance};

    // Initialize accounts
    const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    for (i64 i{1}; i <= num_accounts; ++i) {
        REQUIRE(tree.insert_txn(init_txn, i, helpers::span_from_string("1000")));
    }
    REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(init_txn, lm));

    std::atomic<bool> running{true};
    std::atomic<u64>  aborted_transfers{0};
    std::atomic<u64>  successful_transfers{0};

    helpers::mt_verifier verifier{};

    // Spawn 4 transfer threads
    std::vector<std::jthread> workers;
    for (int thread_idx{0}; thread_idx < 4; ++thread_idx) {
        workers.emplace_back([&, thread_idx] {
            std::mt19937           rng{static_cast<u32>(42 + thread_idx)};
            std::vector<std::byte> buf_from;
            std::vector<std::byte> buf_to;

            while (running) {
                const i64 from{static_cast<i64>(rng() % num_accounts + 1)};
                const i64 to{static_cast<i64>(rng() % num_accounts + 1)};
                if (from == to) { continue; }

                // Transfer 10 credits under SERIALIZABLE
                const auto txn{tm.begin_txn(isolation_level_t::SERIALIZABLE)};
                auto       snap_res{tm.acquire_snapshot(txn)};
                if (!snap_res) {
                    aborted_transfers++;
                    if (!tm.abort_txn(txn, lm)) { verifier.add_failure("abort_txn failed"); }
                    std::this_thread::yield();
                    continue;
                }
                const auto snap{snap_res.value()};

                // Acquire exclusive locks manually for the transfer to avoid write-write conflicts
                const auto from_hash{stdx::hasher{}.combine<i64>(from).finalize()};
                const auto to_hash{stdx::hasher{}.combine<i64>(to).finalize()};

                // Lock in canonical order to prevent lock deadlocks
                bool lock_success{true};
                if (from < to) {
                    lock_success &=
                        lm_lock
                            .lock_row_exclusive(txn, std::to_underlying(tree.tree_id()), from_hash)
                            .has_value();
                    lock_success &=
                        lm_lock.lock_row_exclusive(txn, std::to_underlying(tree.tree_id()), to_hash)
                            .has_value();
                } else {
                    lock_success &=
                        lm_lock.lock_row_exclusive(txn, std::to_underlying(tree.tree_id()), to_hash)
                            .has_value();
                    lock_success &=
                        lm_lock
                            .lock_row_exclusive(txn, std::to_underlying(tree.tree_id()), from_hash)
                            .has_value();
                }

                if (!lock_success) {
                    aborted_transfers++;
                    if (!tm.abort_txn(txn, lm)) { verifier.add_failure("abort_txn failed"); }
                    std::this_thread::yield();
                    continue;
                }

                auto val_from_res{tree.get_txn(txn, snap, tm, from, buf_from)};
                auto val_to_res{tree.get_txn(txn, snap, tm, to, buf_to)};

                if (!val_from_res || !val_to_res) {
                    aborted_transfers++;
                    if (!tm.abort_txn(txn, lm)) { verifier.add_failure("abort_txn failed"); }
                    std::this_thread::yield();
                    continue;
                }
                auto val_from_opt{val_from_res.value()};
                auto val_to_opt{val_to_res.value()};

                if (!val_from_opt || !val_to_opt) {
                    aborted_transfers++;
                    if (!tm.abort_txn(txn, lm)) { verifier.add_failure("abort_txn failed"); }
                    std::this_thread::yield();
                    continue;
                }

                const i64 balance_from{MT_UNWRAP(
                    verifier,
                    helpers::parse_integral<i64>(helpers::string_from_span(*val_from_opt)))};
                const i64 balance_to{MT_UNWRAP(
                    verifier,
                    helpers::parse_integral<i64>(helpers::string_from_span(*val_to_opt)))};

                if (balance_from < 10) {
                    // abort if insufficient funds
                    if (!tm.abort_txn(txn, lm)) { verifier.add_failure("abort_txn failed"); }
                    std::this_thread::yield();
                    continue;
                }

                i64 new_balance_from{balance_from - 10};
                i64 new_balance_to{balance_to + 10};

                if (!tree.update_txn(
                        txn, from, helpers::span_from_string(std::to_string(new_balance_from)))) {
                    verifier.add_failure("update_txn balance_from failed");
                }
                if (!tree.update_txn(
                        txn, to, helpers::span_from_string(std::to_string(new_balance_to)))) {
                    verifier.add_failure("update_txn balance_to failed");
                }

                if (!tm.update_txn_lsn(txn, wal::log::seq_num{2})) {
                    verifier.add_failure("update_txn_lsn failed");
                }
                auto commit_res{tm.commit_txn(txn, lm)};
                if (!commit_res) {
                    aborted_transfers++;
                    if (!tree.rollback_txn(txn)) { verifier.add_failure("rollback_txn failed"); }
                    DISCARD(tm.abort_txn(txn, lm));
                } else {
                    successful_transfers++;
                }

                std::this_thread::yield();
            }
        });
    }

    // Spawn 1 reader thread to check invariants (sum balance should always be total_money)
    std::jthread reader{[&] {
        while (running) {
            i64  sum{0};
            bool read_success{true};
            int  retries{5};

            while (retries-- > 0) {
                sum          = 0;
                read_success = true;
                const auto txn{tm.begin_txn(isolation_level_t::REPEATABLE_READ)};
                auto       snap_res{tm.acquire_snapshot(txn)};
                if (!snap_res) {
                    DISCARD(tm.abort_txn(txn, lm));
                    read_success = false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                const auto snap{snap_res.value()};

                std::vector<std::byte> buf{};
                for (i64 i{1}; i <= num_accounts; ++i) {
                    auto val_opt{tree.get_txn(txn, snap, tm, i, buf)};
                    if (!val_opt) {
                        read_success = false;
                        break;
                    }
                    auto val{val_opt.value()};
                    if (!val) {
                        read_success = false;
                        break;
                    }
                    sum += MT_UNWRAP(verifier,
                                     helpers::parse_integral<i64>(helpers::string_from_span(*val)));
                }

                if (!tm.commit_txn(txn, lm)) { DISCARD(tm.abort_txn(txn, lm)); }

                if (read_success && sum == total_money) { break; }
                if (read_success && sum != total_money) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }

            if (read_success && sum != total_money) {
                verifier.add_failure(
                    fmt::format("stress sum mismatch: {} != {}", sum, total_money));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }};

    // Let it run for 1 second
    std::this_thread::sleep_for(std::chrono::seconds(1));
    running = false;
    workers.clear();
    reader.join();

    // Verify final consistent state
    const auto             final_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    const auto             snap{UNWRAP(tm.acquire_snapshot(final_txn))};
    i64                    sum{0};
    std::vector<std::byte> buf;
    for (i64 i{1}; i <= num_accounts; ++i) {
        const auto val{UNWRAP(tree.get_txn(final_txn, snap, tm, i, buf))};
        sum += UNWRAP(helpers::parse_integral<i64>(helpers::string_from_span(UNWRAP(val))));
    }
    CHECK(sum == total_money);
    REQUIRE(tm.commit_txn(final_txn, lm));

    REQUIRE_FALSE(verifier.dump_if_error());
}

} // namespace cairn::tests
