#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "support/diagnostic/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/undo/manager.hh"

namespace cairn::tests {

using namespace cairn::txn;

TEST_CASE("undo::manager read/write records") {
    helpers::tempfile file{"undo_mgr_test"};
    using pool_t = storage::buffer_pool<64>;
    auto                   pool{UNWRAP(pool_t::open(file.path))};
    undo::manager<i64, 64> undo_mgr{*pool};

    const std::string payload1{"first_version_data"};
    const std::string payload2{"second_version_data"};

    auto ptr1{UNWRAP(undo_mgr.append_record(txn::id_t{10},
                                            100,
                                            undo::op_t::INSERT,
                                            false,
                                            false,
                                            stdx::none,
                                            stdx::none,
                                            helpers::span_from_string(payload1)))};

    auto ptr2{UNWRAP(undo_mgr.append_record(txn::id_t{10},
                                            100,
                                            undo::op_t::UPDATE,
                                            false,
                                            false,
                                            txn::id_t{10},
                                            ptr1,
                                            helpers::span_from_string(payload2)))};
    CHECK(ptr1 != ptr2);

    std::vector<std::byte> rec_payload;
    const auto             rec1{UNWRAP(undo_mgr.read_record(ptr1, rec_payload))};
    CHECK_FALSE(rec1.txn_id);
    CHECK(rec1.key == 100);
    CHECK(rec1.op == undo::op_t::INSERT);
    CHECK(helpers::string_from_span(rec_payload) == payload1);

    const auto rec2{UNWRAP(undo_mgr.read_record(ptr2, rec_payload))};
    CHECK(UNWRAP(rec2.txn_id) == txn::id_t{10});
    CHECK(rec2.key == 100);
    CHECK(rec2.op == undo::op_t::UPDATE);
    CHECK(rec2.prev_undo_ptr == ptr1);
    CHECK(helpers::string_from_span(rec_payload) == payload2);
}

TEST_CASE("undo::manager size checks and page active records") {
    helpers::tempfile file{"undo_mgr_size_test"};
    using pool_t = storage::buffer_pool<64>;
    auto                   pool{UNWRAP(pool_t::open(file.path))};
    undo::manager<i64, 64> undo_mgr{*pool};

    // Oversized record check
    std::vector<std::byte> huge_payload(storage::DB_PAGE_SIZE);
    auto                   err_res{undo_mgr.append_record(
        txn::id_t{1}, 1, undo::op_t::INSERT, false, false, stdx::none, stdx::none, huge_payload)};
    CHECK(err_res.error() == error::STORAGE_TREE_CORRUPT);

    // Active page count and get_page_active_records
    CHECK(undo_mgr.active_page_count() == 0);
    CHECK_FALSE(undo_mgr.get_page_active_records(storage::page_id_t{1}).has_value());

    const std::string payload{"data"};
    auto              ptr1{UNWRAP(undo_mgr.append_record(txn::id_t{1},
                                            10,
                                            undo::op_t::INSERT,
                                            false,
                                            false,
                                            stdx::none,
                                            stdx::none,
                                            helpers::span_from_string(payload)))};
    CHECK(undo_mgr.active_page_count() == 1);
    CHECK(undo_mgr.get_page_active_records(ptr1.page_id) == 1);

    // Reclaim chain and test page dropping
    auto ptr2{UNWRAP(undo_mgr.append_record(txn::id_t{1},
                                            10,
                                            undo::op_t::UPDATE,
                                            false,
                                            false,
                                            stdx::none,
                                            ptr1,
                                            helpers::span_from_string(payload)))};
    CHECK(undo_mgr.get_page_active_records(ptr1.page_id) == 2);

    // Reclaim ptr2's previous ptr (reclaims ptr1)
    REQUIRE(undo_mgr.reclaim_prev_ptr(ptr2));
    CHECK(undo_mgr.get_page_active_records(ptr1.page_id) == 1);

    // Reclaiming again (already reset prev_undo_ptr) is a no-op
    REQUIRE(undo_mgr.reclaim_prev_ptr(ptr2));

    // Reclaim entire chain from ptr2
    undo_mgr.reclaim_undo_chain(ptr2);
    CHECK(undo_mgr.active_page_count() == 0);
}

} // namespace cairn::tests
