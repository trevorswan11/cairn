#include <cstddef>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/txn_id.hh"
#include "wal/log_manager.hh"
#include "wal/log_reader.hh"
#include "wal/log_record.hh"
#include "wal/log_sequence_number.hh"

namespace cairn::tests {

using namespace cairn::storage;
using namespace cairn::wal;

TEST_CASE("WAL page flush forces WAL flush") {
    helpers::tempfile db_file{"wal_int_db"};
    helpers::tempfile log_file{"wal_int_log"};

    auto        bp{helpers::unwrap(buffer_pool<8>::open(db_file.path))};
    log_manager log{log_file.path, 1'024};
    bp->set_log_manager(log);

    page_id_t pid{INVALID_PAGE_ID};
    lsn_t     expected_lsn{INVALID_LSN};
    {
        auto [id, guard]{helpers::unwrap(bp->new_write())};
        pid = id;
        slotted_page sp{*guard.get()};
        sp.refresh_page();

        const std::string_view data{"hello wal integration"};
        log_update_params_t params{.txn_id = txn::txn_id_t{1}, .prev_lsn = stdx::none, .log = log};

        const auto slot_id = helpers::unwrap(sp.insert(helpers::span_from_string(data), params));
        CHECK(slot_id == slot_id_t{0});

        auto page_lsn = guard.get()->page_lsn();
        REQUIRE(page_lsn.has_value());
        expected_lsn = *page_lsn;

        CHECK(log.flushed_lsn() < expected_lsn);
        guard.mark_dirty();
    }

    REQUIRE(bp->flush(pid));
    CHECK(log.flushed_lsn() >= expected_lsn);

    {
        auto reader{helpers::unwrap(log_reader::open(log_file.path))};
        auto r{helpers::unwrap(reader.next())};
        CHECK(r.lsn == expected_lsn);
        CHECK(r.txn_id == txn::txn_id_t{1});
        CHECK(r.type == log_record_type::UPDATE);
        CHECK(r.page_id == pid);
        CHECK(r.slot_id == slot_id_t{0});
        CHECK(helpers::string_from_span(r.redo_data) == "hello wal integration");
    }
}

TEST_CASE("WAL eviction forces WAL flush") {
    helpers::tempfile db_file{"wal_evict_db"};
    helpers::tempfile log_file{"wal_evict_log"};

    auto        bp{helpers::unwrap(buffer_pool<1>::open(db_file.path))};
    log_manager log{log_file.path, 1'024};
    bp->set_log_manager(log);

    lsn_t expected_lsn{INVALID_LSN};
    {
        auto [id, guard]{helpers::unwrap(bp->new_write())};
        slotted_page sp{*guard.get()};
        sp.refresh_page();

        const std::string_view data{"eviction test record"};
        CHECK(sp.insert(helpers::span_from_string(data),
                        {
                            .txn_id   = txn::txn_id_t{42},
                            .prev_lsn = stdx::none,
                            .log      = log,
                        }));
        expected_lsn = *guard.get()->page_lsn();

        CHECK(log.flushed_lsn() < expected_lsn);
        guard.mark_dirty();
    }

    auto [_, guard2]{helpers::unwrap(bp->new_write())};
    CHECK(log.flushed_lsn() >= expected_lsn);
}

} // namespace cairn::tests
