#include <cstddef>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "wal/log/manager.hh"

namespace cairn::tests {

using namespace cairn::storage;
using namespace stdx::size_literals;

TEST_CASE("slotted_page guards when empty") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    CHECK(UNWRAP_ERR(sp.get(slot_id_t{0})) == error_t::STORAGE_INVALID_SLOT);
    CHECK(UNWRAP_ERR(sp.remove(slot_id_t{0})) == error_t::STORAGE_INVALID_SLOT);
    CHECK(UNWRAP_ERR(sp.update(slot_id_t{0}, {})) == error_t::STORAGE_INVALID_SLOT);
}

TEST_CASE("slotted_page insert and get") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    const std::string_view data{"hello world"};
    const auto             slot_id{UNWRAP(sp.insert(helpers::span_from_string(data)))};
    CHECK(slot_id == slot_id_t{0});
    CHECK(sp.slot_count() == 1);

    const auto tuple_out{UNWRAP(sp.get(slot_id))};
    CHECK(helpers::string_from_span(tuple_out) == data);
}

TEST_CASE("slotted_page delete and update") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    const std::string_view data1{"first tuple"};
    const auto             id1{UNWRAP(sp.insert(helpers::span_from_string(data1)))};
    const std::string_view data2{"second tuple"};
    const auto             id2{UNWRAP(sp.insert(helpers::span_from_string(data2)))};

    REQUIRE(sp.remove(id1));
    CHECK(UNWRAP_ERR(sp.get(id1)) == error_t::STORAGE_TUPLE_DELETED);

    // Should reuse tombstone
    const std::string_view data3{"reused tuple"};
    const auto             id3{UNWRAP(sp.insert(helpers::span_from_string(data3)))};
    CHECK(id3 == id1);

    // Update in-place to smaller
    const std::string_view data4{"smaller"};
    REQUIRE(sp.update(id2, helpers::span_from_string(data4)));
    CHECK(helpers::string_from_span(UNWRAP(sp.get(id2))) == data4);

    // Update in-place to larger
    const std::string_view data5{"much much larger tuple"};
    REQUIRE(sp.update(id2, helpers::span_from_string(data5)));
    CHECK(helpers::string_from_span(UNWRAP(sp.get(id2))) == data5);
}

TEST_CASE("slotted_page compaction") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    const std::string data1(2'000, 'a');
    const auto        id1{UNWRAP(sp.insert(helpers::span_from_string(data1)))};
    const std::string data2(2'000, 'b');
    const auto        id2{UNWRAP(sp.insert(helpers::span_from_string(data2)))};
    const std::string data3(2'000, 'c');
    const auto        id3{UNWRAP(sp.insert(helpers::span_from_string(data3)))};

    // Force free space to be made through a compact op
    REQUIRE(sp.remove(id2));
    std::string data4(3'000, 'd');
    REQUIRE(sp.insert(helpers::span_from_string(data4)));

    CHECK(helpers::string_from_span(UNWRAP(sp.get(id1))) == data1);
    CHECK(helpers::string_from_span(UNWRAP(sp.get(id3))) == data3);
}

TEST_CASE("slotted_page write_slot_raw") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    // Case 1. Deletion test
    {
        const std::string_view data{"delete me"};
        const auto             id{UNWRAP(sp.insert(helpers::span_from_string(data)))};
        CHECK(id == slot_id_t{0});
        CHECK(sp.slot_count() == 1);

        REQUIRE(sp.write_slot_raw(id, stdx::none));
        CHECK(UNWRAP_ERR(sp.get(id)) == error_t::STORAGE_TUPLE_DELETED);
        REQUIRE(sp.write_slot_raw(slot_id_t{10}, stdx::none));
    }

    // Case 2. Out-of-bounds insert test
    {
        const std::string_view data{"oob insert"};
        REQUIRE(sp.write_slot_raw(slot_id_t{5}, helpers::span_from_string(data)));
        CHECK(sp.slot_count() == 6);
        for (i32 i{0}; i < 5; ++i) {
            CHECK(UNWRAP_ERR(sp.get(slot_id_t{i})) == error_t::STORAGE_TUPLE_DELETED);
        }
        CHECK(helpers::string_from_span(UNWRAP(sp.get(slot_id_t{5}))) == data);
    }

    // Case 3. In-place update test
    {
        const std::string_view data{"small"};
        REQUIRE(sp.write_slot_raw(slot_id_t{5}, helpers::span_from_string(data)));
        CHECK(helpers::string_from_span(UNWRAP(sp.get(slot_id_t{5}))) == data);
    }

    // Case 4. Out-of-place update test
    {
        const std::string_view data{"much larger tuple data"};
        REQUIRE(sp.write_slot_raw(slot_id_t{5}, helpers::span_from_string(data)));
        CHECK(helpers::string_from_span(UNWRAP(sp.get(slot_id_t{5}))) == data);
    }
}

TEST_CASE("slotted_page logging updates") {
    helpers::tempfile file{"slotted_page_logging"};
    wal::log::manager lm{file.path, 4_KiB};

    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    log_update_params_t log_params{
        .txn_id      = txn::id_t{1},
        .prev_lsn    = stdx::none,
        .log_manager = lm,
    };

    const std::string_view data1{"insert logged"};
    const auto             id{UNWRAP(sp.insert(helpers::span_from_string(data1), log_params))};
    CHECK(id == slot_id_t{0});

    // Update with log
    log_params.prev_lsn = p.page_lsn();
    const std::string_view data2{"update logged"};
    REQUIRE(sp.update(id, helpers::span_from_string(data2), log_params));

    // Remove with log
    log_params.prev_lsn = p.page_lsn();
    REQUIRE(sp.remove(id, log_params));
}

TEST_CASE("slotted_page compaction and full errors during update") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    const std::string data1(1'200, 'a');
    const auto        id1{UNWRAP(sp.insert(helpers::span_from_string(data1)))};
    const std::string data2(1'200, 'b');
    const auto        id2{UNWRAP(sp.insert(helpers::span_from_string(data2)))};
    const std::string data3(1'200, 'c');
    REQUIRE(sp.insert(helpers::span_from_string(data3)));
    REQUIRE(sp.remove(id2)); // Fragment

    // Try to update id1 to 1400 bytes to compact and fit
    const std::string data1_larger(1'400, 'x');
    REQUIRE(sp.update(id1, helpers::span_from_string(data1_larger)));
    CHECK(helpers::string_from_span(UNWRAP(sp.get(id1))) == data1_larger);

    // Try to update id1 to 7000 bytes to compact but still fail
    const std::string data1_too_large(7'000, 'y');
    auto              update_res = sp.update(id1, helpers::span_from_string(data1_too_large));
    CHECK_FALSE(update_res.has_value());
    CHECK(update_res.error() == error_t::STORAGE_PAGE_FULL);
}

} // namespace cairn::tests
