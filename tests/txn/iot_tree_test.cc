#include <cstddef>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/types.hh>

#include "support/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/undo/manager.hh"

namespace cairn::tests {

using namespace cairn::txn;
using helpers::unwrap;

TEST_CASE("txn::iot_tree basic transactional insert, update, delete") {
    helpers::tempfile file{"txn_iot_tree_basic_test"};

    using txn_tree_t = iot_tree<i64, 128, 64>;
    auto                   pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
    undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t             tree{base_tree, undo_mgr};

    const std::string_view val1{"data_one"};
    const std::string_view val2{"data_two"};

    REQUIRE(tree.insert_txn(txn::id_t{1}, 10, helpers::span_from_string(val1)));
    {
        auto [header, payload]{unwrap(tree.get_raw(10))};
        CHECK(header.txn_id == txn::id_t{1});
        CHECK_FALSE(header.is_timestamp);
        CHECK_FALSE(header.is_deleted);
        CHECK_FALSE(header.undo_ptr);
        CHECK(helpers::string_from_span(payload) == val1);
    }

    REQUIRE(tree.update_txn(txn::id_t{2}, 10, helpers::span_from_string(val2)));
    {
        auto [header, payload]{unwrap(tree.get_raw(10))};
        CHECK(header.txn_id == txn::id_t{2});
        CHECK_FALSE(header.is_deleted);

        auto undo_rec = unwrap(undo_mgr.read_record(unwrap(header.undo_ptr)));
        CHECK(undo_rec.record.txn_id == txn::id_t{1});
        CHECK(undo_rec.record.op == undo::op_t::UPDATE);
        CHECK(helpers::string_from_span(undo_rec.payload) == val1);
        CHECK_FALSE(undo_rec.record.prev_undo_ptr);
    }

    REQUIRE(tree.delete_txn(txn::id_t{3}, 10));
    {
        auto [header, payload] = unwrap(tree.get_raw(10));
        CHECK(header.txn_id == txn::id_t{3});
        CHECK(header.is_deleted);
        CHECK(payload.empty());

        auto undo_rec = unwrap(undo_mgr.read_record(unwrap(header.undo_ptr)));
        CHECK(undo_rec.record.txn_id == txn::id_t{2});
        CHECK(undo_rec.record.op == undo::op_t::DELETE);
        CHECK(helpers::string_from_span(undo_rec.payload) == val2);
        CHECK(undo_rec.record.prev_undo_ptr);
    }
}

TEST_CASE("txn::iot_tree rollback transactions") {
    helpers::tempfile file{"txn_iot_tree_rollback_test"};
    using txn_tree_t = iot_tree<i64, 128, 64>;
    auto                   pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                   base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
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
        auto [header, payload]{unwrap(tree.get_raw(2))};
        CHECK(header.txn_id == txn::id_t{20});
        CHECK(helpers::string_from_span(payload) == modified);
    }

    REQUIRE(tree.rollback_txn(txn::id_t{20}));
    {
        auto [header, payload]{unwrap(tree.get_raw(2))};
        CHECK(header.txn_id == txn::id_t{1});
        CHECK_FALSE(header.undo_ptr);
        CHECK(helpers::string_from_span(payload) == original);
    }

    // Scenario 3: Rollback delete (restores tuple)
    REQUIRE(tree.delete_txn(txn::id_t{30}, 2));
    {
        auto [header, payload]{unwrap(tree.get_raw(2))};
        CHECK(header.is_deleted);
    }

    REQUIRE(tree.rollback_txn(txn::id_t{30}));
    {
        auto [header, payload]{unwrap(tree.get_raw(2))};
        CHECK_FALSE(header.is_deleted);
        CHECK(header.txn_id == txn::id_t{1});
        CHECK(helpers::string_from_span(payload) == original);
    }

    // Scenario 4: Multiple updates within the same transaction rollback
    REQUIRE(tree.update_txn(txn::id_t{40}, 2, helpers::span_from_string("first_up")));
    const std::string_view second_up{"second_up"};
    REQUIRE(tree.update_txn(txn::id_t{40}, 2, helpers::span_from_string(second_up)));

    {
        auto [header, payload]{unwrap(tree.get_raw(2))};
        CHECK(header.txn_id == txn::id_t{40});
        CHECK(helpers::string_from_span(payload) == second_up);
    }

    REQUIRE(tree.rollback_txn(txn::id_t{40}));
    {
        auto [header, payload]{unwrap(tree.get_raw(2))};
        CHECK(header.txn_id == txn::id_t{1});
        CHECK(helpers::string_from_span(payload) == original);
    }
}

} // namespace cairn::tests
