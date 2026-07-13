#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "support/diagnostic/error.hh"
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

TEST_CASE("txn::manager begin, update LSN, commit") {
    manager           tm;
    helpers::tempfile file{"txn_manager_ops"};
    wal::log::manager lm{file.path, 1_KiB};

    const auto id1{tm.begin_txn()};
    CHECK(id1 == id_t{1});

    std::vector<wal::checkpoint::att_entry> att;
    tm.snapshot_att(att);
    REQUIRE(att.size() == 1);
    CHECK(att[0].txn_id == id1);
    CHECK(att[0].state == wal::checkpoint::att_entry::state_t::ACTIVE);
    CHECK_FALSE(att[0].last_lsn.has_value());

    // Update LSN
    REQUIRE(tm.update_txn_lsn(id1, wal::log::seq_num{42}));
    tm.snapshot_att(att);
    REQUIRE(att.size() == 1);
    CHECK(att[0].last_lsn == wal::log::seq_num{42});

    // Commit transaction
    REQUIRE(tm.commit_txn(id1, lm));
    tm.snapshot_att(att);
    CHECK(att.empty());

    // Verify the commit record is in WAL
    auto reader{UNWRAP(wal::log::reader::open(file.path))};
    auto r{UNWRAP(UNWRAP(reader.next_record()))};
    CHECK(r.txn_id == id1);
    CHECK(r.type == wal::log::record_type::COMMIT);
    CHECK(r.prev_lsn == wal::log::seq_num{42});
}

TEST_CASE("txn::manager abort transaction") {
    manager           tm;
    helpers::tempfile file{"txn_manager_abort"};
    wal::log::manager lm{file.path, 1_KiB};

    const auto id2{tm.begin_txn()};
    CHECK(id2 == id_t{1});

    REQUIRE(tm.update_txn_lsn(id2, wal::log::seq_num{55}));

    // Abort transaction
    REQUIRE(tm.abort_txn(id2, lm));
    std::vector<wal::checkpoint::att_entry> att;
    tm.snapshot_att(att);
    CHECK(att.empty());

    // Verify the abort record is in WAL
    auto reader{UNWRAP(wal::log::reader::open(file.path))};
    auto r{UNWRAP(UNWRAP(reader.next_record()))};
    CHECK(r.txn_id == id2);
    CHECK(r.type == wal::log::record_type::ABORT);
    CHECK(r.prev_lsn == wal::log::seq_num{55});
}

TEST_CASE("txn::manager set next txn id") {
    manager           tm;
    helpers::tempfile file{"txn_manager_set_next"};
    wal::log::manager lm{file.path, 1_KiB};

    tm.set_next_txn_id(id_t{100});
    const auto id{tm.begin_txn()};
    CHECK(id == id_t{100});
}

TEST_CASE("txn::manager transaction and commit timestamps") {
    manager           tm;
    helpers::tempfile file{"txn_manager_commits"};
    wal::log::manager lm{file.path, 1_KiB};

    const auto id1{tm.begin_txn()};
    CHECK(UNWRAP(tm.get_read_timestamp(id1)) == timestamp_t{0});
    CHECK(tm.snapshot_horizon() == timestamp_t{0});

    const auto id2{tm.begin_txn()};
    CHECK(UNWRAP(tm.get_read_timestamp(id2)) == timestamp_t{0});

    REQUIRE(tm.update_txn_lsn(id1, wal::log::seq_num{10}));
    REQUIRE(tm.commit_txn(id1, lm)); // clock -> 1

    CHECK(UNWRAP(tm.get_commit_timestamp(id1)) == timestamp_t{1});
    CHECK(tm.snapshot_horizon() == timestamp_t{0}); // limited by active id2
    CHECK(UNWRAP(tm.get_commit_timestamp(id1)) == timestamp_t{1});

    REQUIRE(tm.update_txn_lsn(id2, wal::log::seq_num{20}));
    REQUIRE(tm.commit_txn(id2, lm)); // clock -> 2

    CHECK(UNWRAP(tm.get_commit_timestamp(id2)) == timestamp_t{2});
    CHECK(tm.snapshot_horizon() == timestamp_t{2}); // no active txns

    CHECK(UNWRAP(tm.get_commit_timestamp(id1)) == INVALID_TIMESTAMP); // pruned
    CHECK(UNWRAP(tm.get_commit_timestamp(id2)) == timestamp_t{2});    // not pruned

    CHECK(UNWRAP_ERR(tm.get_read_timestamp(id1)) == error::TXN_NOT_FOUND);
}

TEST_CASE("txn::manager not found / error cases") {
    manager           tm;
    helpers::tempfile file{"txn_manager_errors"};
    wal::log::manager lm{file.path, 1_KiB};

    CHECK(UNWRAP_ERR(tm.commit_txn(id_t{999}, lm)) == error::TXN_NOT_FOUND);
    CHECK(UNWRAP_ERR(tm.abort_txn(id_t{999}, lm)) == error::TXN_NOT_FOUND);
    CHECK(UNWRAP_ERR(tm.update_txn_lsn(id_t{999}, wal::log::seq_num{123})) == error::TXN_NOT_FOUND);
}

} // namespace cairn::tests
