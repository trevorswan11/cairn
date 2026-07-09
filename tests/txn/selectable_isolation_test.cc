#include <atomic>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "testhelpers/conversion.hh"
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
using helpers::unwrap;

using txn_tree_t = iot_tree<i64, 128, 64>;

TEST_CASE("txn::selectable_isolation Read Committed snapshot visibility") {
    helpers::tempfile      file{"txn_rc_visibility_test"};
    auto                   pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    helpers::tempfile      wal_file{"txn_rc_visibility_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string("100")));
    REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(init_txn, lm));

    const auto             t1{tm.begin_txn(isolation_level_t::READ_COMMITTED)};
    const auto             snap1{unwrap(tm.acquire_snapshot(t1))};
    std::vector<std::byte> buf;
    CHECK(helpers::string_from_span(unwrap(unwrap(tree.get_txn(t1, snap1, tm, 1, buf)))) == "100");

    // Tx2 concurrently updates key 1 to 200 and commits
    const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.update_txn(t2, 1, helpers::span_from_string("200")));
    REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{2}));
    REQUIRE(tm.commit_txn(t2, lm));

    // Second read under READ_COMMITTED: should see 200
    CHECK(helpers::string_from_span(unwrap(unwrap(tree.get_txn(t1, snap1, tm, 1, buf)))) == "200");
    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{3}));
    REQUIRE(tm.commit_txn(t1, lm));
}

TEST_CASE("txn::selectable_isolation Repeatable Read snapshot visibility") {
    helpers::tempfile      file{"txn_rr_visibility_test"};
    auto                   pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    helpers::tempfile      wal_file{"txn_rr_visibility_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string("100")));
    REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(init_txn, lm));

    const auto             t1{tm.begin_txn(isolation_level_t::REPEATABLE_READ)};
    const auto             snap1{unwrap(tm.acquire_snapshot(t1))};
    std::vector<std::byte> buf;
    CHECK(helpers::string_from_span(unwrap(unwrap(tree.get_txn(t1, snap1, tm, 1, buf)))) == "100");

    // Tx2 concurrently updates key 1 to 200 and commits
    const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.update_txn(t2, 1, helpers::span_from_string("200")));
    REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{2}));
    REQUIRE(tm.commit_txn(t2, lm));

    // Second read under REPEATABLE_READ: should still see 100
    CHECK(helpers::string_from_span(unwrap(unwrap(tree.get_txn(t1, snap1, tm, 1, buf)))) == "100");
    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{3}));
    REQUIRE(tm.commit_txn(t1, lm));
}

TEST_CASE("txn::selectable_isolation Read Committed S-lock release") {
    helpers::tempfile      file{"txn_rc_lock_test"};
    auto                   pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    lock::manager          lm_lock;
    tm.set_lock_manager(lm_lock);

    helpers::tempfile wal_file{"txn_rc_lock_test_wal"};
    wal::log::manager lm{wal_file.path, 1_MiB};

    const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string("100")));
    REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(init_txn, lm));

    const auto             t1{tm.begin_txn(isolation_level_t::READ_COMMITTED)};
    const auto             snap1{unwrap(tm.acquire_snapshot(t1))};
    std::vector<std::byte> buf;
    CHECK(unwrap(tree.get_txn(t1, snap1, tm, 1, buf)).has_value());

    // Tx2 (concurrent) should be able to acquire exclusive lock on key 1 immediately
    // because Tx1 released its S-lock at the end of get_txn.
    const auto t2{tm.begin_txn(isolation_level_t::READ_COMMITTED)};
    const auto key_hash = stdx::hasher{}.combine<i64>(1).finalize();
    REQUIRE(lm_lock.lock_row_exclusive(t2, std::to_underlying(tree.tree_id()), key_hash));

    REQUIRE(tm.commit_txn(t1, lm));
    REQUIRE(tm.commit_txn(t2, lm));
}

TEST_CASE("txn::selectable_isolation Repeatable Read S-lock holding") {
    helpers::tempfile      file{"txn_rr_lock_test"};
    auto                   pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    lock::manager          lm_lock;
    tm.set_lock_manager(lm_lock);

    helpers::tempfile wal_file{"txn_rr_lock_test_wal"};
    wal::log::manager lm{wal_file.path, 1_MiB};

    const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string("100")));
    REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(init_txn, lm));

    const auto             t1{tm.begin_txn(isolation_level_t::REPEATABLE_READ)};
    const auto             snap1{unwrap(tm.acquire_snapshot(t1))};
    std::vector<std::byte> buf;
    CHECK(unwrap(tree.get_txn(t1, snap1, tm, 1, buf)).has_value());

    const auto t2{tm.begin_txn(isolation_level_t::REPEATABLE_READ)};
    const auto key_hash = stdx::hasher{}.combine(1).finalize();

    std::atomic<bool> t2_granted{false};
    std::atomic<bool> t2_started{false};
    {
        std::jthread t{[&] {
            t2_started = true;
            t2_granted =
                lm_lock.lock_row_exclusive(t2, std::to_underlying(tree.tree_id()), key_hash)
                    .has_value();
        }};

        while (!t2_started) { std::this_thread::yield(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(!t2_granted);
        REQUIRE(tm.commit_txn(t1, lm));
    }

    CHECK(t2_granted);
    REQUIRE(tm.commit_txn(t2, lm));
}

} // namespace cairn::tests
