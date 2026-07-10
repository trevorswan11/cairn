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

#include "exec/table_scan.hh"
#include "storage/page.hh"
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

using txn_tree_t   = iot_tree<i64, 128, 64>;
using table_scan_t = exec::table_scan<i64, 128, 64>;

TEST_CASE("selectable isolation write skew prevention") {
    helpers::tempfile      file{"txn_serializable_write_skew_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
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

        const auto snap1{UNWRAP(tm.acquire_snapshot(t1))};
        const auto snap2{UNWRAP(tm.acquire_snapshot(t2))};

        std::vector<std::byte> buf1;
        std::vector<std::byte> buf2;

        // Both read both accounts
        CHECK(UNWRAP(tree.get_txn(t1, snap1, tm, 1, buf1)));
        CHECK(UNWRAP(tree.get_txn(t1, snap1, tm, 2, buf1)));
        CHECK(UNWRAP(tree.get_txn(t2, snap2, tm, 1, buf2)));
        CHECK(UNWRAP(tree.get_txn(t2, snap2, tm, 2, buf2)));

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

        const auto snap1{UNWRAP(tm.acquire_snapshot(t1))};
        const auto snap2{UNWRAP(tm.acquire_snapshot(t2))};

        std::vector<std::byte> buf1;
        std::vector<std::byte> buf2;

        // Both read both accounts
        CHECK(UNWRAP(tree.get_txn(t1, snap1, tm, 1, buf1)));
        CHECK(UNWRAP(tree.get_txn(t1, snap1, tm, 2, buf1)));
        CHECK(UNWRAP(tree.get_txn(t2, snap2, tm, 1, buf2)));
        CHECK(UNWRAP(tree.get_txn(t2, snap2, tm, 2, buf2)));

        // T1 and T2 withdraw respectively
        REQUIRE(tree.update_txn(t1, 1, helpers::span_from_string("0")));
        REQUIRE(tree.update_txn(t2, 2, helpers::span_from_string("0")));

        // T1 commits successfully since it's first
        REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{5}));
        REQUIRE(tm.commit_txn(t1, lm));

        // T2 tries to commit but should fail
        REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{6}));
        CHECK(UNWRAP_ERR(tm.commit_txn(t2, lm)) == error_t::TXN_SERIALIZATION_FAILURE);
    }
}

TEST_CASE("selectable isolation phantom read prevention") {
    helpers::tempfile      file{"txn_serializable_phantom_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    helpers::tempfile      wal_file{"txn_serializable_phantom_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    // Scenario 1: Under SNAPSHOT isolation, phantoms are not detected
    {
        const auto t1{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        const auto snap1{UNWRAP(tm.acquire_snapshot(t1))};

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
        const auto snap1{UNWRAP(tm.acquire_snapshot(t1))};

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
        CHECK(UNWRAP_ERR(tm.commit_txn(t1, lm)) == error_t::TXN_SERIALIZATION_FAILURE);
    }
}

TEST_CASE("selectable isolation Read Committed snapshot visibility") {
    helpers::tempfile      file{"txn_rc_visibility_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
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
    const auto             snap1{UNWRAP(tm.acquire_snapshot(t1))};
    std::vector<std::byte> buf;
    CHECK(helpers::string_from_span(UNWRAP(UNWRAP(tree.get_txn(t1, snap1, tm, 1, buf)))) == "100");

    // Tx2 concurrently updates key 1 to 200 and commits
    const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.update_txn(t2, 1, helpers::span_from_string("200")));
    REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{2}));
    REQUIRE(tm.commit_txn(t2, lm));

    // Second read under READ_COMMITTED: should see 200
    CHECK(helpers::string_from_span(UNWRAP(UNWRAP(tree.get_txn(t1, snap1, tm, 1, buf)))) == "200");
    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{3}));
    REQUIRE(tm.commit_txn(t1, lm));
}

TEST_CASE("selectable isolation Repeatable Read snapshot visibility") {
    helpers::tempfile      file{"txn_rr_visibility_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
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
    const auto             snap1{UNWRAP(tm.acquire_snapshot(t1))};
    std::vector<std::byte> buf;
    CHECK(helpers::string_from_span(UNWRAP(UNWRAP(tree.get_txn(t1, snap1, tm, 1, buf)))) == "100");

    // Tx2 concurrently updates key 1 to 200 and commits
    const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.update_txn(t2, 1, helpers::span_from_string("200")));
    REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{2}));
    REQUIRE(tm.commit_txn(t2, lm));

    // Second read under REPEATABLE_READ: should still see 100
    CHECK(helpers::string_from_span(UNWRAP(UNWRAP(tree.get_txn(t1, snap1, tm, 1, buf)))) == "100");
    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{3}));
    REQUIRE(tm.commit_txn(t1, lm));
}

TEST_CASE("selectable isolation Read Committed S-lock release") {
    helpers::tempfile      file{"txn_rc_lock_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
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
    const auto             snap1{UNWRAP(tm.acquire_snapshot(t1))};
    std::vector<std::byte> buf;
    CHECK(UNWRAP(tree.get_txn(t1, snap1, tm, 1, buf)).has_value());

    // Tx2 (concurrent) should be able to acquire exclusive lock on key 1 immediately
    // because Tx1 released its S-lock at the end of get_txn.
    const auto t2{tm.begin_txn(isolation_level_t::READ_COMMITTED)};
    const auto key_hash = stdx::hasher{}.combine<i64>(1).finalize();
    REQUIRE(lm_lock.lock_row_exclusive(t2, std::to_underlying(tree.tree_id()), key_hash));

    REQUIRE(tm.commit_txn(t1, lm));
    REQUIRE(tm.commit_txn(t2, lm));
}

TEST_CASE("selectable isolation Repeatable Read S-lock holding") {
    helpers::tempfile      file{"txn_rr_lock_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
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
    const auto             snap1{UNWRAP(tm.acquire_snapshot(t1))};
    std::vector<std::byte> buf;
    CHECK(UNWRAP(tree.get_txn(t1, snap1, tm, 1, buf)).has_value());

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

TEST_CASE("selectable isolation undo page reclamation") {
    helpers::tempfile      file{"txn_undo_reclaim_test"};
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    undo_mgr.set_txn_manager(tm);
    helpers::tempfile wal_file{"txn_undo_reclaim_test_wal"};
    wal::log::manager lm{wal_file.path, 1_MiB};

    // Write first version of key 1 and commit
    const auto init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string("100")));
    REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(init_txn, lm));

    // Start a transaction t_pinned which will pin the horizon
    const auto t_pinned{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    const auto snap_pinned{UNWRAP(tm.acquire_snapshot(t_pinned))};

    const auto t1{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.update_txn(t1, 1, helpers::span_from_string("200")));
    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{2}));
    REQUIRE(tm.commit_txn(t1, lm));

    const auto t2{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.update_txn(t2, 1, helpers::span_from_string("300")));
    REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{3}));
    REQUIRE(tm.commit_txn(t2, lm));

    CHECK(undo_mgr.active_page_count() > 0);
    storage::page_id_t old_page_id;
    {
        const auto get{UNWRAP(tree.tree().get(1))};
        const auto header{cairn::txn::read_version_header(get)};
        REQUIRE(header.undo_ptr);
        old_page_id = header.undo_ptr->page_id;
    }

    // Commit the pinned transaction and prune committed transaction history
    REQUIRE(tm.commit_txn(t_pinned, lm));
    tm.prune_committed_txns(tm.snapshot_horizon());

    // Since all previous versions are now older than the snapshot horizon,
    // resolve_version should trigger unlinking and reclamation of the old undo chain.
    const auto             t_reader{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    const auto             snap_reader{UNWRAP(tm.acquire_snapshot(t_reader))};
    std::vector<std::byte> buf;
    const auto             res{UNWRAP(UNWRAP(tree.get_txn(t_reader, snap_reader, tm, 1, buf)))};
    CHECK(helpers::string_from_span(res) == "300");
    REQUIRE(tm.commit_txn(t_reader, lm));
    CHECK(undo_mgr.active_page_count() == 1);

    // Write to key 1 will see that version 300 is now committed before the horizon, so it will
    // unlink and reclaim Record 2 during the update.
    const auto t3{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.update_txn(t3, 1, helpers::span_from_string("400")));
    REQUIRE(tm.update_txn_lsn(t3, wal::log::seq_num{4}));
    REQUIRE(tm.commit_txn(t3, lm));
    CHECK(UNWRAP(undo_mgr.get_page_active_records(old_page_id)) == 1);
    CHECK(undo_mgr.active_page_count() == 1);
}

} // namespace cairn::tests
