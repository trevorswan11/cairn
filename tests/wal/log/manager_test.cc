#include <atomic>
#include <cstddef>
#include <filesystem>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "helpers/mock_records.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "wal/log/manager.hh"
#include "wal/log/reader.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::wal;
using namespace stdx::size_literals;

TEST_CASE("log::manager append and flush") {
    helpers::tempfile file{"log_manager_basic"};

    {
        log::manager manager{file.path, 1_KiB};

        log::record rec1;
        rec1.txn_id = txn::id_t{1};
        rec1.type   = log::record_type::BEGIN;
        auto lsn1   = helpers::unwrap(manager.append_record(rec1));

        log::record rec2;
        rec2.txn_id    = txn::id_t{1};
        rec2.type      = log::record_type::UPDATE;
        rec2.page_id   = storage::page_id_t{4};
        rec2.slot_id   = storage::slot_id_t{2};
        rec2.redo_data = helpers::redo_bytes;
        rec2.undo_data = helpers::undo_bytes;
        auto lsn2{helpers::unwrap(manager.append_record(rec2))};

        CHECK(lsn1 == log::seq_num{1});
        CHECK(lsn2 == log::seq_num{2});

        // Flush up to lsn2
        CHECK(manager.flush(lsn2));
        CHECK(manager.flushed_lsn() >= lsn2);
    }

    // Verify written data by reading it back
    {
        auto reader{helpers::unwrap(log::reader::open(file.path))};
        auto r1{helpers::unwrap(helpers::unwrap(reader.next_record()))};
        CHECK(r1.lsn == log::seq_num{1});
        CHECK(r1.type == log::record_type::BEGIN);

        auto r2{helpers::unwrap(helpers::unwrap(reader.next_record()))};
        CHECK(r2.lsn == log::seq_num{2});
        CHECK(r2.type == log::record_type::UPDATE);
        CHECK(r2.page_id == storage::page_id_t{4});
        CHECK(r2.slot_id == storage::slot_id_t{2});
    }
}

TEST_CASE("log::manager double buffering boundary") {
    helpers::tempfile file{"log_manager_boundary"};

    {
        log::manager manager{file.path, 128};

        // Append multiple records to force multiple buffer swaps
        std::vector<log::seq_num> lsns;
        for (i64 i{0}; i < 10; ++i) {
            log::record rec;
            rec.txn_id    = txn::id_t{i};
            rec.type      = log::record_type::UPDATE;
            rec.page_id   = storage::page_id_t{i};
            rec.slot_id   = storage::slot_id_t{0};
            rec.redo_data = helpers::redo_bytes;
            rec.undo_data = helpers::undo_bytes;

            lsns.emplace_back(helpers::unwrap(manager.append_record(rec)));
        }

        // Flush all
        CHECK(manager.flush(lsns.back()));
    }

    // Verify all 10 records are read back successfully
    {
        auto reader{helpers::unwrap(log::reader::open(file.path))};
        for (i64 i{0}; i < 10; ++i) {
            auto r{helpers::unwrap(helpers::unwrap(reader.next_record()))};
            CHECK(r.lsn == log::seq_num{i + 1});
            CHECK(r.txn_id == txn::id_t{i});
            CHECK(r.page_id == storage::page_id_t{i});
        }
    }
}

TEST_CASE("log::manager concurrent appends") {
    helpers::tempfile file{"log_manager_concurrent"};

    const i32 num_threads{8};
    const i32 appends_per_thread{50};

    {
        log::manager manager{file.path, 4_KiB};

        std::vector<std::jthread> workers;
        std::atomic<bool>         go{false};

        for (i32 t{0}; t < num_threads; ++t) {
            workers.emplace_back([&manager, t, &go] {
                while (!go.load()) { std::this_thread::yield(); }

                for (i32 i{0}; i < appends_per_thread; ++i) {
                    log::record rec;
                    rec.txn_id    = txn::id_t{t};
                    rec.type      = log::record_type::UPDATE;
                    rec.page_id   = storage::page_id_t{i};
                    rec.slot_id   = storage::slot_id_t{0};
                    rec.redo_data = helpers::redo_bytes;

                    helpers::unwrap(manager.flush(helpers::unwrap(manager.append_record(rec))));
                }
            });
        }

        go.store(true);
    }

    // Read back and verify count and content consistency
    {
        i32  total_records{0};
        auto reader{helpers::unwrap(log::reader::open(file.path))};
        while (helpers::unwrap(reader.next_record())) { total_records++; }
        CHECK(total_records == num_threads * appends_per_thread);
    }
}

} // namespace cairn::tests
