#include <cstddef>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
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
using helpers::unwrap;
using helpers::unwrap_err;

using txn_tree_t = iot_tree<i64, 128, 64>;
using pool_t     = storage::buffer_pool<64>;

TEST_CASE(
    "transaction integration: serialization conflict, lock release, and rollback verification") {
    helpers::tempfile db_file{"txn_integration_db"};
    helpers::tempfile log_file{"txn_integration_log"};

    wal::log::manager lm{log_file.path, 1_MiB};
    auto              pool{unwrap(pool_t::open(db_file.path))};
    pool->set_log_manager(lm);

    auto                   base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    lock::manager          lm_lock;
    tm.set_lock_manager(lm_lock);

    // Initialize key 1 with "init_val"
    const std::string_view key1_init_val{"init_val"};
    const auto             init_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
    REQUIRE(tree.insert_txn(init_txn, 1, helpers::span_from_string(key1_init_val)));
    REQUIRE(tm.update_txn_lsn(init_txn, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(init_txn, lm));

    // Run concurrent transactions with conflicts and check rollback
    helpers::mt_verifier    verifier;
    deterministic_scheduler scheduler{{tid0, tid1, tid0, tid1, tid0, tid1, tid0, tid1}};

    const std::string_view t1_val{"val_t1"};
    const std::string_view t3_val{"val_t3"};

    {
        std::jthread t0{[&] {
            const auto t1{tm.begin_txn(isolation_level_t::SERIALIZABLE)};
            THREAD_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            const auto             snap1{unwrap(verifier, tm.acquire_snapshot(t1))};
            std::vector<std::byte> buf;
            auto                   val1{unwrap(verifier, tree.get_txn(t1, snap1, tm, 1, buf))};
            THREAD_CHECK(verifier,
                         helpers::string_from_span(unwrap(verifier, val1)) == key1_init_val);

            THREAD_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));
            THREAD_REQUIRE(verifier, tree.update_txn(t1, 1, helpers::span_from_string(t1_val)));
            THREAD_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));

            // Commit T1: should succeed
            THREAD_REQUIRE(verifier, tm.update_txn_lsn(t1, wal::log::seq_num{2}));
            THREAD_CHECK(verifier, tm.commit_txn(t1, lm).has_value());

            THREAD_REQUIRE(verifier, scheduler.yield_to_schedule(tid0));
            scheduler.thread_exit(tid0);
        }};

        std::jthread t1{[&] {
            THREAD_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));
            const auto t2{tm.begin_txn(isolation_level_t::SERIALIZABLE)};

            const auto             snap2{unwrap(verifier, tm.acquire_snapshot(t2))};
            std::vector<std::byte> buf;
            auto                   val2{unwrap(verifier, tree.get_txn(t2, snap2, tm, 1, buf))};
            THREAD_CHECK(verifier,
                         helpers::string_from_span(unwrap(verifier, val2)) == key1_init_val);

            THREAD_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));
            THREAD_REQUIRE(verifier, tree.update_txn(t2, 1, helpers::span_from_string("val_t2")));
            THREAD_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));

            // Commit T2: must fail with serialization failure due to T1 writing first
            THREAD_REQUIRE(verifier, tm.update_txn_lsn(t2, wal::log::seq_num{3}));
            THREAD_CHECK(verifier,
                         unwrap_err(verifier, tm.commit_txn(t2, lm)) ==
                             error_t::TXN_SERIALIZATION_FAILURE);
            THREAD_REQUIRE(verifier, tree.rollback_txn(t2));
            THREAD_REQUIRE(verifier, tm.abort_txn(t2, lm));

            THREAD_REQUIRE(verifier, scheduler.yield_to_schedule(tid1));
            scheduler.thread_exit(tid1);
        }};
    }
    REQUIRE_FALSE(verifier.dump_if_error());

    // Verify database state after rollback
    {
        const auto             check_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        const auto             check_snap{unwrap(tm.acquire_snapshot(check_txn))};
        std::vector<std::byte> buf;

        // Key 1: T1 committed data ("val_t1") must be present (and not T2's rolled-back "val_t2")
        auto val{unwrap(tree.get_txn(check_txn, check_snap, tm, 1, buf))};
        CHECK(helpers::string_from_span(unwrap(val)) == t1_val);
    }

    // Verify that locks were correctly released by executing a new transaction on key 1
    {
        const auto t3{tm.begin_txn(isolation_level_t::SERIALIZABLE)};
        REQUIRE(tree.update_txn(t3, 1, helpers::span_from_string(t3_val)));
        REQUIRE(tm.update_txn_lsn(t3, wal::log::seq_num{4}));
        REQUIRE(tm.commit_txn(t3, lm));

        // Verify the update succeeded
        const auto             check_txn{tm.begin_txn(isolation_level_t::SNAPSHOT)};
        const auto             check_snap{unwrap(tm.acquire_snapshot(check_txn))};
        std::vector<std::byte> buf;
        auto                   val{unwrap(tree.get_txn(check_txn, check_snap, tm, 1, buf))};
        CHECK(helpers::string_from_span(unwrap(val)) == t3_val);
    }
}

} // namespace cairn::tests
