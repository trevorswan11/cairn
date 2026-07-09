#include <cstddef>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "exec/table_scan.hh"
#include "support/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::txn;
using namespace cairn::exec;
using namespace stdx::size_literals;
using helpers::unwrap;
using helpers::unwrap_err;

using txn_tree_t   = iot_tree<i64, 128, 64>;
using table_scan_t = table_scan<i64, 128, 64>;

TEST_CASE("txn::serializable write skew prevention") {
    helpers::tempfile      file{"txn_serializable_write_skew_test"};
    auto                   pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    helpers::tempfile      wal_file{"txn_serializable_write_skew_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    // Initialize two keys with value 100
    const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string("100")));
    REQUIRE(tree.insert_txn(init_txn, 2, helpers::span_from_string("100")));
    REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(init_txn, lm));

    // Scenario 1: Under SNAPSHOT isolation, write skew is allowed
    {
        const auto t1{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};

        const auto snap1{unwrap(tm.acquire_snapshot(t1))};
        const auto snap2{unwrap(tm.acquire_snapshot(t2))};

        std::vector<std::byte> buf1;
        std::vector<std::byte> buf2;

        // Both read both accounts
        CHECK(unwrap(tree.get_txn(t1, snap1, tm, 1, buf1)));
        CHECK(unwrap(tree.get_txn(t1, snap1, tm, 2, buf1)));
        CHECK(unwrap(tree.get_txn(t2, snap2, tm, 1, buf2)));
        CHECK(unwrap(tree.get_txn(t2, snap2, tm, 2, buf2)));

        // T1 and T2 withdraw respectively
        REQUIRE(tree.update_txn(t1, 1, helpers::span_from_string("0")));
        REQUIRE(tree.update_txn(t2, 2, helpers::span_from_string("0")));

        // Both commit successfully under SI
        REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{2}));
        REQUIRE(tm.commit_txn(t1, lm));

        REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{3}));
        REQUIRE(tm.commit_txn(t2, lm));
    }

    // Restore state to 100 for accounts 1 and 2
    {
        const auto restore_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        REQUIRE(tree.update_txn(restore_txn, 1, helpers::span_from_string("100")));
        REQUIRE(tree.update_txn(restore_txn, 2, helpers::span_from_string("100")));
        REQUIRE(tm.update_txn_lsn(restore_txn, wal::log::seq_num{4}));
        REQUIRE(tm.commit_txn(restore_txn, lm));
    }

    // Scenario 2: Under SERIALIZABLE isolation, one transaction must abort
    {
        const auto t1{tm.begin_txn(isolation_level_t::SERIALIZABLE)};
        const auto t2{tm.begin_txn(isolation_level_t::SERIALIZABLE)};

        const auto snap1{unwrap(tm.acquire_snapshot(t1))};
        const auto snap2{unwrap(tm.acquire_snapshot(t2))};

        std::vector<std::byte> buf1;
        std::vector<std::byte> buf2;

        // Both read both accounts
        CHECK(unwrap(tree.get_txn(t1, snap1, tm, 1, buf1)));
        CHECK(unwrap(tree.get_txn(t1, snap1, tm, 2, buf1)));
        CHECK(unwrap(tree.get_txn(t2, snap2, tm, 1, buf2)));
        CHECK(unwrap(tree.get_txn(t2, snap2, tm, 2, buf2)));

        // T1 and T2 withdraw respectively
        REQUIRE(tree.update_txn(t1, 1, helpers::span_from_string("0")));
        REQUIRE(tree.update_txn(t2, 2, helpers::span_from_string("0")));

        // T1 commits successfully since it's first
        REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{5}));
        REQUIRE(tm.commit_txn(t1, lm));

        // T2 tries to commit but should fail
        REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{6}));
        CHECK(unwrap_err(tm.commit_txn(t2, lm)) == error_t::TXN_SERIALIZATION_FAILURE);
    }
}

TEST_CASE("txn::serializable phantom read prevention") {
    helpers::tempfile      file{"txn_serializable_phantom_test"};
    auto                   pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    helpers::tempfile      wal_file{"txn_serializable_phantom_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    // Scenario 1: Under SNAPSHOT isolation, phantoms are not detected
    {
        const auto t1{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        const auto snap1{unwrap(tm.acquire_snapshot(t1))};

        table_scan_t scanner{tree, t1, snap1, tm};
        usize        count{0};
        REQUIRE(scanner(10, 20, [&](const auto&, auto) { count++; }));
        CHECK(count == 0);

        // T2 concurrent transaction inserts key 15 and commits
        const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        REQUIRE(tree.insert_txn(t2, 15, helpers::span_from_string("val15")));
        REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{1}));
        REQUIRE(tm.commit_txn(t2, lm));

        // T1 commits successfully under SNAPSHOT
        REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{2}));
        REQUIRE(tm.commit_txn(t1, lm));
    }

    // Clean up key 15
    {
        const auto cleanup_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        REQUIRE(tree.delete_txn(cleanup_txn, 15));
        REQUIRE(tm.update_txn_lsn(cleanup_txn, wal::log::seq_num{3}));
        REQUIRE(tm.commit_txn(cleanup_txn, lm));
    }

    // Scenario 2: Under SERIALIZABLE isolation, phantoms are detected
    {
        const auto t1{tm.begin_txn(isolation_level_t::SERIALIZABLE)};
        const auto snap1{unwrap(tm.acquire_snapshot(t1))};

        table_scan_t scanner{tree, t1, snap1, tm};
        usize        count{0};
        REQUIRE(scanner(10, 20, [&](const auto&, auto) { count++; }));
        CHECK(count == 0);

        // T2 concurrent transaction inserts key 15 and commits
        const auto t2{tm.begin_txn(isolation_level_t::SERIALIZABLE)};
        REQUIRE(tree.insert_txn(t2, 15, helpers::span_from_string("val15")));
        REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{4}));
        REQUIRE(tm.commit_txn(t2, lm));

        // T1 tries to commit but should fail
        REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{5}));
        CHECK(unwrap_err(tm.commit_txn(t1, lm)) == error_t::TXN_SERIALIZATION_FAILURE);
    }
}

} // namespace cairn::tests
