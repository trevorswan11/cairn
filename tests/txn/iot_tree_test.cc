#include <cstddef>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "support/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::txn;

TEST_CASE("txn::iot_tree basic transactional insert, update, delete") {
    helpers::tempfile file{"txn_iot_tree_basic_test"};

    using txn_tree_t = iot_tree<i64, 128, 64>;
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};

    const std::string_view val1{"data_one"};
    const std::string_view val2{"data_two"};

    REQUIRE(tree.insert_txn(txn::id_t{1}, 10, helpers::span_from_string(val1)));
    {
        auto [header, payload]{UNWRAP(tree.get_raw(10))};
        CHECK(UNWRAP(header.txn_id) == txn::id_t{1});
        CHECK_FALSE(header.is_timestamp);
        CHECK_FALSE(header.is_deleted);
        CHECK_FALSE(header.undo_ptr);
        CHECK(helpers::string_from_span(payload) == val1);
    }

    REQUIRE(tree.update_txn(txn::id_t{2}, 10, helpers::span_from_string(val2)));
    {
        auto [header, payload]{UNWRAP(tree.get_raw(10))};
        CHECK(UNWRAP(header.txn_id) == txn::id_t{2});
        CHECK_FALSE(header.is_deleted);

        std::vector<std::byte> rec_payload;
        const auto undo_rec{UNWRAP(undo_mgr.read_record(UNWRAP(header.undo_ptr), rec_payload))};
        CHECK(UNWRAP(undo_rec.txn_id) == txn::id_t{1});
        CHECK(undo_rec.op == undo::op_t::UPDATE);
        CHECK(helpers::string_from_span(rec_payload) == val1);
        CHECK_FALSE(undo_rec.prev_undo_ptr);
    }

    REQUIRE(tree.delete_txn(txn::id_t{3}, 10));
    {
        auto [header, payload]{UNWRAP(tree.get_raw(10))};
        CHECK(UNWRAP(header.txn_id) == txn::id_t{3});
        CHECK(header.is_deleted);
        CHECK(payload.empty());

        std::vector<std::byte> rec_payload;
        const auto undo_rec{UNWRAP(undo_mgr.read_record(UNWRAP(header.undo_ptr), rec_payload))};
        CHECK(UNWRAP(undo_rec.txn_id) == txn::id_t{2});
        CHECK(undo_rec.op == undo::op_t::DELETE);
        CHECK(helpers::string_from_span(rec_payload) == val2);
        CHECK(undo_rec.prev_undo_ptr);
    }
}

TEST_CASE("txn::iot_tree rollback transactions") {
    helpers::tempfile file{"txn_iot_tree_rollback_test"};
    using txn_tree_t = iot_tree<i64, 128, 64>;
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};

    // Scenario 1: Rollback insert (removes tuple)
    REQUIRE(tree.insert_txn(txn::id_t{10}, 1, helpers::span_from_string("hello")));
    REQUIRE(tree.rollback_txn(txn::id_t{10}));
    CHECK(tree.get_raw(1).error() == error_t::STORAGE_KEY_NOT_FOUND);

    // Scenario 2: Rollback update (restores old value/header)
    const std::string_view original{"original"};
    REQUIRE(tree.insert_txn(txn::id_t{1}, 2, helpers::span_from_string(original)));
    const std::string_view modified{"modified"};
    REQUIRE(tree.update_txn(txn::id_t{20}, 2, helpers::span_from_string(modified)));

    {
        auto [header, payload]{UNWRAP(tree.get_raw(2))};
        CHECK(UNWRAP(header.txn_id) == txn::id_t{20});
        CHECK(helpers::string_from_span(payload) == modified);
    }

    REQUIRE(tree.rollback_txn(txn::id_t{20}));
    {
        auto [header, payload]{UNWRAP(tree.get_raw(2))};
        CHECK(UNWRAP(header.txn_id) == txn::id_t{1});
        CHECK_FALSE(header.undo_ptr);
        CHECK(helpers::string_from_span(payload) == original);
    }

    // Scenario 3: Rollback delete (restores tuple)
    REQUIRE(tree.delete_txn(txn::id_t{30}, 2));
    {
        auto [header, payload]{UNWRAP(tree.get_raw(2))};
        CHECK(header.is_deleted);
    }

    REQUIRE(tree.rollback_txn(txn::id_t{30}));
    {
        auto [header, payload]{UNWRAP(tree.get_raw(2))};
        CHECK_FALSE(header.is_deleted);
        CHECK(UNWRAP(header.txn_id) == txn::id_t{1});
        CHECK(helpers::string_from_span(payload) == original);
    }

    // Scenario 4: Multiple updates within the same transaction rollback
    REQUIRE(tree.update_txn(txn::id_t{40}, 2, helpers::span_from_string("first_up")));
    const std::string_view second_up{"second_up"};
    REQUIRE(tree.update_txn(txn::id_t{40}, 2, helpers::span_from_string(second_up)));

    {
        auto [header, payload]{UNWRAP(tree.get_raw(2))};
        CHECK(UNWRAP(header.txn_id) == txn::id_t{40});
        CHECK(helpers::string_from_span(payload) == second_up);
    }

    REQUIRE(tree.rollback_txn(txn::id_t{40}));
    {
        auto [header, payload]{UNWRAP(tree.get_raw(2))};
        CHECK(UNWRAP(header.txn_id) == txn::id_t{1});
        CHECK(helpers::string_from_span(payload) == original);
    }
}

TEST_CASE("txn::iot_tree snapshot isolation and visibility reads") {
    using namespace stdx::size_literals;

    helpers::tempfile file{"txn_iot_tree_snapshot_test"};
    using txn_tree_t = iot_tree<i64, 128, 64>;
    auto                   pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};
    manager                tm;
    helpers::tempfile      wal_file{"txn_iot_tree_snapshot_test_wal"};
    wal::log::manager      lm{wal_file.path, 1_MiB};

    // Transaction 1 starts and inserts a key
    const auto             t1{tm.begin_txn()};
    const std::string_view v1_data{"v1"};
    REQUIRE(tree.insert_txn(t1, 1, helpers::span_from_string(v1_data)));

    // Transaction 2 starts. It shouldn't see v1 because T1 is active
    const auto             t2{tm.begin_txn()};
    const auto             snap2{UNWRAP(tm.acquire_snapshot(t2))};
    std::vector<std::byte> buf;

    auto get2{UNWRAP(tree.get_txn(t2, snap2, tm, 1, buf))};
    CHECK_FALSE(get2.has_value());

    // Transaction 1 commits
    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(t1, lm));

    // Transaction 2 reads again. It STILL shouldn't see it (Repeatable Read)
    get2 = UNWRAP(tree.get_txn(t2, snap2, tm, 1, buf));
    CHECK_FALSE(get2.has_value());

    // Transaction 3 starts. It SHOULD see v1
    const auto t3{tm.begin_txn()};
    const auto snap3{UNWRAP(tm.acquire_snapshot(t3))};
    const auto get3{UNWRAP(UNWRAP(tree.get_txn(t3, snap3, tm, 1, buf)))};
    CHECK(helpers::string_from_span(get3) == v1_data);
    REQUIRE(tree.update_txn(t3, 1, helpers::span_from_string("v2")));

    // Transaction 4 starts. It should see "v1" because T3 is active
    const auto t4{tm.begin_txn()};
    const auto snap4{UNWRAP(tm.acquire_snapshot(t4))};
    auto       get4{UNWRAP(UNWRAP(tree.get_txn(t4, snap4, tm, 1, buf)))};
    CHECK(helpers::string_from_span(get4) == v1_data);
}

} // namespace cairn::tests
