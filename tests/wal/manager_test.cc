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
#include "support/error.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "wal/manager.hh"
#include "wal/reader.hh"
#include "wal/record.hh"
#include "wal/sequence_number.hh"

namespace cairn::tests {

using namespace cairn::wal;
using namespace stdx::size_literals;

TEST_CASE("log_manager append and flush") {
    helpers::tempfile file{"log_manager_basic"};

    {
        manager manager{file.path, 1_KiB};

        record rec1;
        rec1.txn_id = txn::id_t{1};
        rec1.type   = record_type::BEGIN;
        auto lsn1   = helpers::unwrap(manager.append_record(rec1));

        record rec2;
        rec2.txn_id    = txn::id_t{1};
        rec2.type      = record_type::UPDATE;
        rec2.page_id   = storage::page_id_t{4};
        rec2.slot_id   = storage::slot_id_t{2};
        rec2.redo_data = helpers::redo_bytes;
        rec2.undo_data = helpers::undo_bytes;
        auto lsn2{helpers::unwrap(manager.append_record(rec2))};

        CHECK(lsn1 == lsn_t{1});
        CHECK(lsn2 == lsn_t{2});

        // Flush up to lsn2
        CHECK(manager.flush(lsn2));
        CHECK(manager.flushed_lsn() >= lsn2);
    }

    // Verify written data by reading it back
    {
        auto reader{helpers::unwrap(reader::open(file.path))};
        auto r1{helpers::unwrap(reader.next())};
        CHECK(r1.lsn == lsn_t{1});
        CHECK(r1.type == record_type::BEGIN);

        auto r2{helpers::unwrap(reader.next())};
        CHECK(r2.lsn == lsn_t{2});
        CHECK(r2.type == record_type::UPDATE);
        CHECK(r2.page_id == storage::page_id_t{4});
        CHECK(r2.slot_id == storage::slot_id_t{2});
    }
}

TEST_CASE("log_manager double buffering boundary") {
    helpers::tempfile file{"log_manager_boundary"};

    {
        manager manager{file.path, 128};

        // Append multiple records to force multiple buffer swaps
        std::vector<lsn_t> lsns;
        for (i64 i{0}; i < 10; ++i) {
            record rec;
            rec.txn_id    = txn::id_t{i};
            rec.type      = record_type::UPDATE;
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
        auto reader{helpers::unwrap(reader::open(file.path))};
        for (i64 i{0}; i < 10; ++i) {
            auto r{helpers::unwrap(reader.next())};
            CHECK(r.lsn == lsn_t{i + 1});
            CHECK(r.txn_id == txn::id_t{i});
            CHECK(r.page_id == storage::page_id_t{i});
        }
    }
}

TEST_CASE("log_manager concurrent appends") {
    helpers::tempfile file{"log_manager_concurrent"};
    std::filesystem::remove(file.path);

    const i32 num_threads{8};
    const i32 appends_per_thread{50};

    {
        manager manager{file.path, 4_KiB};

        std::vector<std::jthread> workers;
        std::atomic<bool>         go{false};

        for (i32 t{0}; t < num_threads; ++t) {
            workers.emplace_back([&manager, t, &go]() {
                while (!go.load()) { std::this_thread::yield(); }

                for (i32 i{0}; i < appends_per_thread; ++i) {
                    record rec;
                    rec.txn_id    = txn::id_t{t};
                    rec.type      = record_type::UPDATE;
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
        auto reader{helpers::unwrap(reader::open(file.path))};
        i32  total_records{0};
        while (true) {
            auto res{reader.next()};
            if (!res.has_value() && res.error() == error_t::WAL_EOF) { break; }
            REQUIRE(res.has_value());
            total_records++;
        }
        CHECK(total_records == num_threads * appends_per_thread);
    }
}

} // namespace cairn::tests
