#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "support/error.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/manager.hh"
#include "wal/checkpoint/types.hh"
#include "wal/log/manager.hh"
#include "wal/log/reader.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::txn;
using namespace stdx::size_literals;
using helpers::unwrap;

TEST_CASE("txn::manager basics") {
    manager           tm;
    helpers::tempfile file{"txn_manager_test"};
    wal::log::manager lm{file.path, 1_KiB};

    SECTION("begin, update LSN, commit") {
        const auto id1{tm.begin_txn()};
        CHECK(id1 == id_t{1});

        auto att{tm.snapshot_att()};
        REQUIRE(att.size() == 1);
        CHECK(att[0].txn_id == id1);
        CHECK(att[0].state == wal::checkpoint::att_entry::state_t::ACTIVE);
        CHECK_FALSE(att[0].last_lsn.has_value());

        // Update LSN
        REQUIRE(tm.update_txn_lsn(id1, wal::log::seq_num{42}));
        att = tm.snapshot_att();
        REQUIRE(att.size() == 1);
        CHECK(att[0].last_lsn == wal::log::seq_num{42});

        // Commit transaction
        REQUIRE(tm.commit_txn(id1, lm));
        CHECK(tm.snapshot_att().empty());

        // Verify the commit record is in WAL
        auto reader{unwrap(wal::log::reader::open(file.path))};
        auto r{unwrap(unwrap(reader.next_record()))};
        CHECK(r.txn_id == id1);
        CHECK(r.type == wal::log::record_type::COMMIT);
        CHECK(r.prev_lsn == wal::log::seq_num{42});
    }

    SECTION("abort transaction") {
        const auto id2{tm.begin_txn()};
        CHECK(id2 == id_t{1});

        REQUIRE(tm.update_txn_lsn(id2, wal::log::seq_num{55}));

        // Abort transaction
        REQUIRE(tm.abort_txn(id2, lm));
        CHECK(tm.snapshot_att().empty());

        // Verify the abort record is in WAL
        auto reader{unwrap(wal::log::reader::open(file.path))};
        auto r{unwrap(unwrap(reader.next_record()))};
        CHECK(r.txn_id == id2);
        CHECK(r.type == wal::log::record_type::ABORT);
        CHECK(r.prev_lsn == wal::log::seq_num{55});
    }

    SECTION("set next txn id") {
        tm.set_next_txn_id(id_t{100});
        const auto id{tm.begin_txn()};
        CHECK(id == id_t{100});
    }

    SECTION("transaction and commit timestamps") {
        const auto id1{tm.begin_txn()};
        CHECK(unwrap(tm.get_read_timestamp(id1)) == timestamp_t{0});
        CHECK(tm.snapshot_horizon() == timestamp_t{0});

        const auto id2{tm.begin_txn()};
        CHECK(unwrap(tm.get_read_timestamp(id2)) == timestamp_t{0});

        REQUIRE(tm.update_txn_lsn(id1, wal::log::seq_num{10}));
        REQUIRE(tm.commit_txn(id1, lm)); // clock -> 1

        CHECK(unwrap(tm.get_commit_timestamp(id1)) == timestamp_t{1});
        CHECK(tm.snapshot_horizon() == timestamp_t{0}); // limited by active id2
        CHECK(unwrap(tm.get_commit_timestamp(id1)) == timestamp_t{1});

        REQUIRE(tm.update_txn_lsn(id2, wal::log::seq_num{20}));
        REQUIRE(tm.commit_txn(id2, lm)); // clock -> 2

        CHECK(unwrap(tm.get_commit_timestamp(id2)) == timestamp_t{2});
        CHECK(tm.snapshot_horizon() == timestamp_t{2}); // no active txns

        CHECK(unwrap(tm.get_commit_timestamp(id1)) == INVALID_TIMESTAMP); // pruned
        CHECK(unwrap(tm.get_commit_timestamp(id2)) == timestamp_t{2});    // not pruned

        CHECK(helpers::unwrap_err(tm.get_read_timestamp(id1)) == error_t::TXN_NOT_FOUND);
    }

    SECTION("not found / error cases") {
        CHECK(helpers::unwrap_err(tm.commit_txn(id_t{999}, lm)) == error_t::TXN_NOT_FOUND);
        CHECK(helpers::unwrap_err(tm.abort_txn(id_t{999}, lm)) == error_t::TXN_NOT_FOUND);
        CHECK(helpers::unwrap_err(tm.update_txn_lsn(id_t{999}, wal::log::seq_num{123})) ==
              error_t::TXN_NOT_FOUND);
    }
}

} // namespace cairn::tests
