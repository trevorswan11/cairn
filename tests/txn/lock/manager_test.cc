#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "support/error.hh"
#include "testhelpers/tempfile.hh"
#include "txn/id.hh"
#include "txn/lock/manager.hh"
#include "txn/lock/types.hh"
#include "txn/manager.hh"
#include "wal/log/manager.hh"

namespace cairn::tests {

using namespace txn;
using namespace stdx::size_literals;

TEST_CASE("lock::manager basic compatibility") {
    lock::manager          lm{};
    const id_t             tx1{1};
    const id_t             tx2{2};
    const lock::index_id_t idx{1};

    // S/S compatibility
    REQUIRE(lm.lock_row_shared(tx1, idx, 100));
    REQUIRE(lm.lock_row_shared(tx2, idx, 100));

    lm.release_all_locks(tx1);
    lm.release_all_locks(tx2);

    // Reentrancy and upgrade
    REQUIRE(lm.lock_row_shared(tx1, idx, 100));
    REQUIRE(lm.lock_row_shared(tx1, idx, 100));    // reentrant S
    REQUIRE(lm.lock_row_exclusive(tx1, idx, 100)); // upgrade to X
    REQUIRE(lm.lock_row_exclusive(tx1, idx, 100)); // reentrant X

    lm.release_all_locks(tx1);
}

TEST_CASE("lock::manager blocking and release") {
    lock::manager          lm{};
    const id_t             tx1{1};
    const id_t             tx2{2}; // tx2 is younger, will wait
    const lock::index_id_t idx{1};

    // tx1 acquires Exclusive lock
    REQUIRE(lm.lock_row_exclusive(tx1, idx, 100));
    std::atomic<bool> tx2_granted{false};
    std::atomic<bool> tx2_started{false};

    {
        std::jthread t{[&] {
            tx2_started = true;
            auto res{lm.lock_row_shared(tx2, idx, 100)};
            if (res) { tx2_granted = true; }
        }};

        while (!tx2_started) { std::this_thread::yield(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(tx2_granted); // Blocked

        lm.release_all_locks(tx1);
    }

    CHECK(tx2_granted);
}

TEST_CASE("lock::manager wound-wait deadlock prevention") {
    lock::manager          lm{};
    const id_t             tx_old{1};
    const id_t             tx_young{2};
    const lock::index_id_t idx{1};

    REQUIRE(lm.lock_row_exclusive(tx_young, idx, 200));
    REQUIRE(lm.lock_row_exclusive(tx_old, idx, 100));

    std::atomic<bool>    young_granted{false};
    std::atomic<error_t> young_err{error_t::IO_ERROR};
    {
        std::atomic<bool> young_started{false};
        std::jthread      t_young{[&] {
            young_started = true;
            if (auto res{lm.lock_row_exclusive(tx_young, idx, 100)}) {
                young_granted = true;
            } else {
                young_err = res.error();
                lm.release_all_locks(tx_young);
            }
        }};

        while (!young_started) { std::this_thread::yield(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(young_granted);

        // tx_old (older) requests lock on Resource 200 -> wounds tx_young
        REQUIRE(lm.lock_row_exclusive(tx_old, idx, 200));
    }

    CHECK_FALSE(young_granted);
    CHECK(young_err == error_t::TXN_DEADLOCK_DETECTED);
}

TEST_CASE("lock::manager range/gap locking") {
    lock::manager          lm{};
    const id_t             tx1{1};
    const id_t             tx2{2};
    const lock::index_id_t idx{1};

    // Range scan on [10, 20] would have shared locks on gaps and rows
    REQUIRE(lm.lock_row_shared(tx1, idx, 10));
    REQUIRE(lm.lock_row_shared(tx1, idx, 20));
    REQUIRE(lm.lock_gap_shared(tx1, idx, 20)); // gap before 20 (10, 20)

    std::atomic<bool> insert_granted{false};
    {
        std::atomic<bool> insert_started{false};
        std::jthread      t{[&] {
            insert_started = true;
            if (lm.lock_gap_exclusive(tx2, idx, 20)) { // insert into gap (10, 20)
                insert_granted = true;
            }
        }};

        while (!insert_started) { std::this_thread::yield(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(insert_granted); // Blocked by gap lock

        lm.release_all_locks(tx1);
    }

    CHECK(insert_granted);
}

TEST_CASE("lock::manager automatic release on txn manager commit/abort") {
    manager           tm{};
    lock::manager     lm{};
    helpers::tempfile file{"txn_manager_locks_auto"};
    wal::log::manager log_mgr{file.path, 1_KiB};
    tm.set_lock_manager(lm);

    const auto             id1{tm.begin_txn()};
    const auto             id2{tm.begin_txn()};
    const lock::index_id_t idx{1};
    REQUIRE(lm.lock_row_exclusive(id1, idx, 100));

    // id2 tries to lock 100 and blocks
    std::atomic<bool> tx2_granted{false};
    {
        std::atomic<bool> tx2_started{false};
        std::jthread      t{[&] {
            tx2_started = true;
            if (lm.lock_row_exclusive(id2, idx, 100)) { tx2_granted = true; }
        }};

        while (!tx2_started) { std::this_thread::yield(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(tx2_granted);

        // Commit id1 should automatically release lock on 100
        REQUIRE(tm.commit_txn(id1, log_mgr));
    }

    CHECK(tx2_granted);
}

} // namespace cairn::tests
