#include <cstddef>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "wal/log/manager.hh"
#include "wal/log/reader.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace stdx::size_literals;

TEST_CASE("WAL page flush forces WAL flush") {
    helpers::tempfile db_file{"wal_int_db"};
    helpers::tempfile log_file{"wal_int_log"};

    wal::log::manager log{log_file.path, 1_KiB};
    auto              bp{helpers::unwrap(storage::buffer_pool<8>::open(db_file.path))};
    bp->set_log_manager(log);

    storage::page_id_t pid;
    wal::log::seq_num  expected_lsn;
    {
        auto [id, guard]{helpers::unwrap(bp->new_write())};
        pid = id;
        storage::slotted_page sp{*guard.get()};
        sp.refresh_page();

        const std::string_view data{"hello wal integration"};
        const auto             slot_id = helpers::unwrap(sp.insert(helpers::span_from_string(data),
                                                                   {
                                                                       .txn_id      = txn::id_t{1},
                                                                       .prev_lsn    = stdx::none,
                                                                       .log_manager = log,
                                                       }));
        CHECK(slot_id == storage::slot_id_t{0});

        auto page_lsn = guard.get()->page_lsn();
        REQUIRE(page_lsn.has_value());
        expected_lsn = *page_lsn;

        CHECK(log.flushed_lsn() < expected_lsn);
        guard.mark_dirty();
    }

    REQUIRE(bp->flush(pid));
    CHECK(log.flushed_lsn() >= expected_lsn);

    {
        auto reader{helpers::unwrap(wal::log::reader::open(log_file.path))};
        auto r{helpers::unwrap(helpers::unwrap(reader.next_record()))};
        CHECK(r.lsn == expected_lsn);
        CHECK(r.txn_id == txn::id_t{1});
        CHECK(r.type == wal::log::record_type::UPDATE);
        CHECK(r.page_id == pid);
        CHECK(r.slot_id == storage::slot_id_t{0});
        CHECK(helpers::string_from_span(r.redo_data) == "hello wal integration");
    }
}

TEST_CASE("WAL eviction forces WAL flush") {
    helpers::tempfile db_file{"wal_evict_db"};
    helpers::tempfile log_file{"wal_evict_log"};

    wal::log::manager log{log_file.path, 1_KiB};
    auto              bp{helpers::unwrap(storage::buffer_pool<1>::open(db_file.path))};
    bp->set_log_manager(log);

    wal::log::seq_num expected_lsn;
    {
        auto [id, guard]{helpers::unwrap(bp->new_write())};
        storage::slotted_page sp{*guard.get()};
        sp.refresh_page();

        const std::string_view data{"eviction test record"};
        CHECK(sp.insert(helpers::span_from_string(data),
                        {
                            .txn_id      = txn::id_t{42},
                            .prev_lsn    = stdx::none,
                            .log_manager = log,
                        }));
        expected_lsn = *guard.get()->page_lsn();

        CHECK(log.flushed_lsn() < expected_lsn);
        guard.mark_dirty();
    }

    auto [_, guard2]{helpers::unwrap(bp->new_write())};
    CHECK(log.flushed_lsn() >= expected_lsn);
}

} // namespace cairn::tests
