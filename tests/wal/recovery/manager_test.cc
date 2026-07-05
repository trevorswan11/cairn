#include <cstddef>
#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/manager.hh"
#include "wal/checkpoint/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"
#include "wal/recovery/manager.hh"

namespace cairn::tests {

using namespace cairn::storage;
using namespace cairn::txn;
using namespace cairn::wal;
using namespace stdx::size_literals;

TEST_CASE("recovery clean shutdown (no-op)") {
    helpers::tempfile db_file{"recovery_clean_db"};
    helpers::tempfile log_file{"recovery_clean_log"};
    helpers::tempfile control_file{"recovery_clean_control"};

    using pool_t = buffer_pool<8>;
    page_id_t              pid;
    log::seq_num           update_lsn;
    const std::string_view data{"clean shutdown record"};

    // Run database cleanly and shut down
    {
        log::manager log{log_file.path, 4_KiB};
        auto         bp{helpers::unwrap(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager tm;
        const auto   tid{tm.begin_txn()};

        auto [id, guard]{helpers::unwrap(bp->new_write())};
        pid = id;

        slotted_page sp{*guard.get()};
        sp.refresh_page();

        CHECK(sp.insert(helpers::span_from_string(data),
                        {
                            .txn_id      = tid,
                            .prev_lsn    = stdx::none,
                            .log_manager = log,
                        }));

        update_lsn = helpers::unwrap(guard.get()->page_lsn());
        REQUIRE(tm.update_txn_lsn(tid, update_lsn));
        guard.mark_dirty();
        guard.drop();

        REQUIRE(tm.commit_txn(tid, log));
    }

    // Run recovery manager on the cleanly shut down database
    {
        log::manager log{log_file.path, 4_KiB};
        auto         bp{helpers::unwrap(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager         tm;
        recovery::manager<8> rm{*bp, tm, log, control_file.path, log_file.path};
        REQUIRE(rm.recover());
    }

    // Verify data is still intact
    {
        auto         bp{helpers::unwrap(pool_t::open(db_file.path))};
        auto         guard{helpers::unwrap(bp->fetch_read(pid))};
        slotted_page sp{*guard.get()};

        const auto tuple{helpers::unwrap(sp.get(slot_id_t{0}))};
        CHECK(helpers::string_from_span(tuple) == data);
    }
}

TEST_CASE("recovery crash") {
    helpers::tempfile db_file{"recovery_crash_db"};
    helpers::tempfile log_file{"recovery_crash_log"};
    helpers::tempfile control_file{"recovery_crash_control"};
    helpers::tempfile db_snapshot{"recovery_crash_db_snapshot"};

    using pool_t = buffer_pool<8>;
    page_id_t pid1;
    page_id_t pid2;

    // Perform database operations
    {
        log::manager log{log_file.path, 16_KiB};
        auto         bp{helpers::unwrap(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager           tm;
        const std::string_view t1_data{"T1 committed data"};
        const auto             t1{tm.begin_txn()};
        const std::string_view t2_data{"T2 uncommitted data"};
        const auto             t2{tm.begin_txn()};

        // Write to page 1 under T1
        {
            auto [id, guard]{helpers::unwrap(bp->new_write())};
            pid1 = id;
            slotted_page sp{*guard.get()};
            sp.refresh_page();

            CHECK(sp.insert(helpers::span_from_string(t1_data),
                            {
                                .txn_id      = t1,
                                .prev_lsn    = stdx::none,
                                .log_manager = log,
                            }));
            REQUIRE(tm.update_txn_lsn(t1, helpers::unwrap(guard.get()->page_lsn())));
            guard.mark_dirty();
        }

        // Write to page 2 under T2
        {
            auto [id, guard]{helpers::unwrap(bp->new_write())};
            pid2 = id;
            slotted_page sp{*guard.get()};
            sp.refresh_page();

            CHECK(sp.insert(helpers::span_from_string(t2_data),
                            {
                                .txn_id      = t2,
                                .prev_lsn    = stdx::none,
                                .log_manager = log,
                            }));
            REQUIRE(tm.update_txn_lsn(t2, helpers::unwrap(guard.get()->page_lsn())));
            guard.mark_dirty();
        }

        // T1 commits, t2 does NOT
        REQUIRE(tm.commit_txn(t1, log));
        std::filesystem::copy_file(
            db_file.path, db_snapshot.path, std::filesystem::copy_options::overwrite_existing);
    }

    // Run recovery manager on the crashed database file
    {
        std::filesystem::copy_file(
            db_snapshot.path, db_file.path, std::filesystem::copy_options::overwrite_existing);

        log::manager log{log_file.path, 16_KiB};
        auto         bp{helpers::unwrap(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager         tm;
        recovery::manager<8> rm{*bp, tm, log, control_file.path, log_file.path};
        REQUIRE(rm.recover());
    }

    // Verify recovered state
    {
        auto bp{helpers::unwrap(pool_t::open(db_file.path))};

        // T1 committed data must be present
        {
            auto         guard{helpers::unwrap(bp->fetch_read(pid1))};
            slotted_page sp{*guard.get()};
            const auto   tuple{helpers::unwrap(sp.get(slot_id_t{0}))};
            CHECK(helpers::string_from_span(tuple) == "T1 committed data");
        }

        // T2 uncommitted data must NOT be present
        {
            auto         guard{helpers::unwrap(bp->fetch_read(pid2))};
            slotted_page sp{*guard.get()};
            CHECK(helpers::unwrap_err(sp.get(slot_id_t{0})) == error_t::STORAGE_TUPLE_DELETED);
        }
    }
}

TEST_CASE("recovery with checkpoint") {
    helpers::tempfile db_file{"recovery_checkpoint_db"};
    helpers::tempfile log_file{"recovery_checkpoint_log"};
    helpers::tempfile control_file{"recovery_checkpoint_control"};
    helpers::tempfile db_snapshot{"recovery_checkpoint_db_snapshot"};

    using pool_t = buffer_pool<8>;
    page_id_t    pid1;
    page_id_t    pid2;
    log::seq_num update_lsn1;
    log::seq_num update_lsn2;

    {
        log::manager log{log_file.path, 16_KiB};
        auto         bp{helpers::unwrap(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager           tm;
        checkpoint::manager<8> cm{control_file.path};

        const auto t1{tm.begin_txn()};
        const auto t2{tm.begin_txn()};

        // 1. Write T1 page 1
        {
            auto [id, guard]{helpers::unwrap(bp->new_write())};
            pid1 = id;
            slotted_page sp{*guard.get()};
            sp.refresh_page();
            CHECK(sp.insert(helpers::span_from_string("T1 checkpoinable data"),
                            {
                                .txn_id      = t1,
                                .prev_lsn    = stdx::none,
                                .log_manager = log,
                            }));
            update_lsn1 = helpers::unwrap(guard.get()->page_lsn());
            REQUIRE(tm.update_txn_lsn(t1, update_lsn1));
            guard.mark_dirty();
        }

        // 2. Perform a checkpoint
        const auto cp_lsn = helpers::unwrap(cm.checkpoint(*bp, tm, log));
        REQUIRE(cp_lsn != log::INVALID_LSN);

        // 3. Write T2 page 2
        {
            auto [id, guard]{helpers::unwrap(bp->new_write())};
            pid2 = id;
            slotted_page sp{*guard.get()};
            sp.refresh_page();
            CHECK(sp.insert(helpers::span_from_string("T2 post-checkpoint data"),
                            {
                                .txn_id      = t2,
                                .prev_lsn    = stdx::none,
                                .log_manager = log,
                            }));
            update_lsn2 = helpers::unwrap(guard.get()->page_lsn());
            REQUIRE(tm.update_txn_lsn(t2, update_lsn2));
            guard.mark_dirty();
        }

        // 4. Commit T1 with T2 uncommitted
        REQUIRE(tm.commit_txn(t1, log));
        std::filesystem::copy_file(
            db_file.path, db_snapshot.path, std::filesystem::copy_options::overwrite_existing);
    }

    // Run recovery on crashed state
    {
        std::filesystem::copy_file(
            db_snapshot.path, db_file.path, std::filesystem::copy_options::overwrite_existing);

        log::manager log{log_file.path, 16_KiB};
        auto         bp{helpers::unwrap(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager         tm;
        recovery::manager<8> rm{*bp, tm, log, control_file.path, log_file.path};
        REQUIRE(rm.recover());
    }

    // Verify state
    {
        auto bp{helpers::unwrap(pool_t::open(db_file.path))};

        // T1 committed data must be present
        {
            auto         guard{helpers::unwrap(bp->fetch_read(pid1))};
            slotted_page sp{*guard.get()};
            const auto   tuple{helpers::unwrap(sp.get(slot_id_t{0}))};
            CHECK(helpers::string_from_span(tuple) == "T1 checkpoinable data");
        }

        // T2 uncommitted data must NOT be present
        {
            auto         guard{helpers::unwrap(bp->fetch_read(pid2))};
            slotted_page sp{*guard.get()};
            CHECK(helpers::unwrap_err(sp.get(slot_id_t{0})) == error_t::STORAGE_TUPLE_DELETED);
        }
    }
}

} // namespace cairn::tests
