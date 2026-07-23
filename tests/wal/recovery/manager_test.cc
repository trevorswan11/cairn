#include <cstddef>
#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/diagnostic/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/manager.hh"
#include "wal/checkpoint/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/record.hh"
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
        auto         bp{UNWRAP(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager tm;
        const auto   tid{tm.begin_txn()};

        auto [id, guard]{UNWRAP(bp->new_write())};
        pid = id;

        slotted_page sp{*guard.get()};
        sp.refresh_page();

        CHECK(sp.insert(helpers::span_from_string(data),
                        {
                            .txn_id      = tid,
                            .prev_lsn    = stdx::none,
                            .log_manager = log,
                        }));

        update_lsn = UNWRAP(guard.get()->page_lsn());
        REQUIRE(tm.update_txn_lsn(tid, update_lsn));
        guard.mark_dirty();
        guard.drop();

        REQUIRE(tm.commit_txn(tid, log));
    }

    // Run recovery manager on the cleanly shut down database
    {
        log::manager log{log_file.path, 4_KiB};
        auto         bp{UNWRAP(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager         tm;
        recovery::manager<8> rm{*bp, tm, log, control_file.path, log_file.path};
        REQUIRE(rm.recover());
    }

    // Verify data is still intact
    {
        auto         bp{UNWRAP(pool_t::open(db_file.path))};
        auto         guard{UNWRAP(bp->fetch_read(pid))};
        slotted_page sp{*guard.get()};

        const auto tuple{UNWRAP(sp.get(slot_id_t{0}))};
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
        auto         bp{UNWRAP(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager           tm;
        const std::string_view t1_data{"T1 committed data"};
        const auto             t1{tm.begin_txn()};
        const std::string_view t2_data{"T2 uncommitted data"};
        const auto             t2{tm.begin_txn()};

        // Write to page 1 under T1
        {
            auto [id, guard]{UNWRAP(bp->new_write())};
            pid1 = id;
            slotted_page sp{*guard.get()};
            sp.refresh_page();

            CHECK(sp.insert(helpers::span_from_string(t1_data),
                            {
                                .txn_id      = t1,
                                .prev_lsn    = stdx::none,
                                .log_manager = log,
                            }));
            REQUIRE(tm.update_txn_lsn(t1, UNWRAP(guard.get()->page_lsn())));
            guard.mark_dirty();
        }

        // Write to page 2 under T2
        {
            auto [id, guard]{UNWRAP(bp->new_write())};
            pid2 = id;
            slotted_page sp{*guard.get()};
            sp.refresh_page();

            CHECK(sp.insert(helpers::span_from_string(t2_data),
                            {
                                .txn_id      = t2,
                                .prev_lsn    = stdx::none,
                                .log_manager = log,
                            }));
            REQUIRE(tm.update_txn_lsn(t2, UNWRAP(guard.get()->page_lsn())));
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
        auto         bp{UNWRAP(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager         tm;
        recovery::manager<8> rm{*bp, tm, log, control_file.path, log_file.path};
        REQUIRE(rm.recover());
    }

    // Verify recovered state
    {
        auto bp{UNWRAP(pool_t::open(db_file.path))};

        // T1 committed data must be present
        {
            auto         guard{UNWRAP(bp->fetch_read(pid1))};
            slotted_page sp{*guard.get()};
            const auto   tuple{UNWRAP(sp.get(slot_id_t{0}))};
            CHECK(helpers::string_from_span(tuple) == "T1 committed data");
        }

        // T2 uncommitted data must NOT be present
        {
            auto         guard{UNWRAP(bp->fetch_read(pid2))};
            slotted_page sp{*guard.get()};
            CHECK(UNWRAP_ERR(sp.get(slot_id_t{0})) == error::STORAGE_TUPLE_DELETED);
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
        auto         bp{UNWRAP(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager        tm;
        checkpoint::manager cm{control_file.path};

        const auto t1{tm.begin_txn()};
        const auto t2{tm.begin_txn()};

        // 1. Write T1 page 1
        {
            auto [id, guard]{UNWRAP(bp->new_write())};
            pid1 = id;
            slotted_page sp{*guard.get()};
            sp.refresh_page();
            CHECK(sp.insert(helpers::span_from_string("T1 checkpoinable data"),
                            {
                                .txn_id      = t1,
                                .prev_lsn    = stdx::none,
                                .log_manager = log,
                            }));
            update_lsn1 = UNWRAP(guard.get()->page_lsn());
            REQUIRE(tm.update_txn_lsn(t1, update_lsn1));
            guard.mark_dirty();
        }

        // 2. Perform a checkpoint
        const auto cp_lsn{UNWRAP(cm.checkpoint(*bp, tm, log))};
        REQUIRE(cp_lsn != log::INVALID_LSN);

        // 3. Write T2 page 2
        {
            auto [id, guard]{UNWRAP(bp->new_write())};
            pid2 = id;
            slotted_page sp{*guard.get()};
            sp.refresh_page();
            CHECK(sp.insert(helpers::span_from_string("T2 post-checkpoint data"),
                            {
                                .txn_id      = t2,
                                .prev_lsn    = stdx::none,
                                .log_manager = log,
                            }));
            update_lsn2 = UNWRAP(guard.get()->page_lsn());
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
        auto         bp{UNWRAP(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager         tm;
        recovery::manager<8> rm{*bp, tm, log, control_file.path, log_file.path};
        REQUIRE(rm.recover());
    }

    // Verify state
    {
        auto bp{UNWRAP(pool_t::open(db_file.path))};

        // T1 committed data must be present
        {
            auto         guard{UNWRAP(bp->fetch_read(pid1))};
            slotted_page sp{*guard.get()};
            const auto   tuple{UNWRAP(sp.get(slot_id_t{0}))};
            CHECK(helpers::string_from_span(tuple) == "T1 checkpoinable data");
        }

        // T2 uncommitted data must NOT be present
        {
            auto         guard{UNWRAP(bp->fetch_read(pid2))};
            slotted_page sp{*guard.get()};
            CHECK(UNWRAP_ERR(sp.get(slot_id_t{0})) == error::STORAGE_TUPLE_DELETED);
        }
    }
}

TEST_CASE("recovery with missing log file") {
    helpers::tempfile db_file{"recovery_nolog_db"};
    helpers::tempfile log_file{"recovery_nolog_log"};
    helpers::tempfile control_file{"recovery_nolog_control"};

    using pool_t = buffer_pool<8>;
    auto         pool{UNWRAP(pool_t::open(db_file.path))};
    txn::manager tm;

    // Remove log_file so it doesn't exist
    std::filesystem::remove(log_file.path);

    log::manager         log_mgr{log_file.path, 4_KiB};
    recovery::manager<8> rm{*pool, tm, log_mgr, control_file.path, log_file.path};
    REQUIRE(rm.recover());
}

TEST_CASE("recovery with CLEAR log records") {
    helpers::tempfile db_file{"recovery_clear_db"};
    helpers::tempfile log_file{"recovery_clear_log"};
    helpers::tempfile control_file{"recovery_clear_control"};

    using pool_t = buffer_pool<8>;
    page_id_t pid;

    {
        log::manager log{log_file.path, 16_KiB};
        auto         bp{UNWRAP(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        txn::manager tm;
        const auto   t1{tm.begin_txn()};

        auto [id, guard]{UNWRAP(bp->new_write())};
        pid = id;
        slotted_page sp{*guard.get()};
        sp.refresh_page();

        CHECK(sp.insert(helpers::span_from_string("initial data"),
                        {
                            .txn_id      = t1,
                            .prev_lsn    = stdx::none,
                            .log_manager = log,
                        }));
        const auto update_lsn{UNWRAP(guard.get()->page_lsn())};

        // Append a CLEAR record directly for t1
        log::record clr;
        clr.txn_id        = t1;
        clr.type          = log::record_type::CLEAR;
        clr.page_id       = pid;
        clr.slot_id       = slot_id_t{0};
        clr.prev_lsn      = update_lsn;
        clr.undo_next_lsn = stdx::none;
        clr.redo_data     = helpers::span_from_string("cleared data");

        const auto clr_lsn{UNWRAP(log.append_record(clr))};
        DISCARD(sp.write_slot_raw(slot_id_t{0}, helpers::span_from_string("cleared data")));
        guard.get()->set_page_lsn(clr_lsn);
        guard.mark_dirty();
    }

    // Recover with active t1
    {
        log::manager log{log_file.path, 16_KiB};
        auto         bp{UNWRAP(pool_t::open(db_file.path))};
        bp->set_log_manager(log);

        // t1 was not committed, so undo will process CLEAR / BEGIN
        txn::manager         tm;
        recovery::manager<8> rm{*bp, tm, log, control_file.path, log_file.path};
        REQUIRE(rm.recover());
    }
}

} // namespace cairn::tests
