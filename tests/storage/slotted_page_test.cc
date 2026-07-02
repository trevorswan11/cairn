#include <cstddef>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>

#include "storage/error.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::storage;

namespace {

[[nodiscard]] auto span_from_string(std::string_view str) -> gsl::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(str.data()), str.size()};
}

[[nodiscard]] auto string_from_span(gsl::span<const std::byte> span) -> std::string_view {
    return {reinterpret_cast<const char*>(span.data()), span.size()};
}

} // namespace

TEST_CASE("slotted_page guards when empty") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    CHECK(helpers::unwrap_err(sp.get(slot_id_t{0})) == error_t::INVALID_SLOT);
    CHECK(helpers::unwrap_err(sp.remove(slot_id_t{0})) == error_t::INVALID_SLOT);
    CHECK(helpers::unwrap_err(sp.update(slot_id_t{0}, {})) == error_t::INVALID_SLOT);
}

TEST_CASE("slotted_page insert and get") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    const std::string_view data{"hello world"};
    const auto             slot_id{helpers::unwrap(sp.insert(span_from_string(data)))};
    CHECK(slot_id == slot_id_t{0});
    CHECK(sp.slot_count() == 1);

    const auto tuple_out{helpers::unwrap(sp.get(slot_id))};
    CHECK(string_from_span(tuple_out) == data);
}

TEST_CASE("slotted_page delete and update") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    const std::string_view data1{"first tuple"};
    const auto             id1{helpers::unwrap(sp.insert(span_from_string(data1)))};
    const std::string_view data2{"second tuple"};
    const auto             id2{helpers::unwrap(sp.insert(span_from_string(data2)))};

    REQUIRE(sp.remove(id1));
    CHECK(helpers::unwrap_err(sp.get(id1)) == error_t::TUPLE_DELETED);

    // Should reuse tombstone
    const std::string_view data3{"reused tuple"};
    const auto             id3{helpers::unwrap(sp.insert(span_from_string(data3)))};
    CHECK(id3 == id1);

    // Update in-place to smaller
    const std::string_view data4{"smaller"};
    REQUIRE(sp.update(id2, span_from_string(data4)));
    CHECK(string_from_span(helpers::unwrap(sp.get(id2))) == data4);

    // Update in-place to larger
    const std::string_view data5{"much much larger tuple"};
    REQUIRE(sp.update(id2, span_from_string(data5)));
    CHECK(string_from_span(helpers::unwrap(sp.get(id2))) == data5);
}

TEST_CASE("slotted_page compaction") {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    const std::string data1(2'000, 'a');
    const auto        id1{helpers::unwrap(sp.insert(span_from_string(data1)))};
    const std::string data2(2'000, 'b');
    const auto        id2{helpers::unwrap(sp.insert(span_from_string(data2)))};
    const std::string data3(2'000, 'c');
    const auto        id3{helpers::unwrap(sp.insert(span_from_string(data3)))};

    // Force free space to be made through a compact op
    REQUIRE(sp.remove(id2));
    std::string data4(3'000, 'd');
    REQUIRE(sp.insert(span_from_string(data4)));

    CHECK(string_from_span(helpers::unwrap(sp.get(id1))) == data1);
    CHECK(string_from_span(helpers::unwrap(sp.get(id3))) == data3);
}

} // namespace cairn::tests
