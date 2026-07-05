#include "sql/type.hh"
#include "sql/value.hh"
#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>
#include <string_view>

namespace cairn::tests {

using namespace cairn::sql;

TEST_CASE("sql::value basics") {
    SECTION("Boolean") {
        value_t v{true};
        CHECK(v.type() == type::id_t::BOOLEAN);
        CHECK(!v.is_null());
        CHECK(v.get_value().as<bool>() == true);
    }

    SECTION("Tinyint") {
        value_t v{i8{42}};
        CHECK(v.type() == type::id_t::TINYINT);
        CHECK(!v.is_null());
        CHECK(v.get_value().as<i8>() == 42);
    }

    SECTION("Smallint") {
        value_t v{i16{4'242}};
        CHECK(v.type() == type::id_t::SMALLINT);
        CHECK(!v.is_null());
        CHECK(v.get_value().as<i16>() == 4'242);
    }

    SECTION("Integer") {
        value_t v{i32{424'242}};
        CHECK(v.type() == type::id_t::INTEGER);
        CHECK(!v.is_null());
        CHECK(v.get_value().as<i32>() == 424'242);
    }

    SECTION("Bigint") {
        value_t v{i64{4'242'424'242}};
        CHECK(v.type() == type::id_t::BIGINT);
        CHECK(!v.is_null());
        CHECK(v.get_value().as<i64>() == 4'242'424'242);
    }

    SECTION("Float") {
        value_t v{f32{42.5f}};
        CHECK(v.type() == type::id_t::FLOAT);
        CHECK(!v.is_null());
        CHECK(v.get_value().as<f32>() == 42.5f);
    }

    SECTION("Double") {
        value_t v{double{42.5}};
        CHECK(v.type() == type::id_t::DOUBLE);
        CHECK(!v.is_null());
        CHECK(v.get_value().as<double>() == 42.5);
    }

    SECTION("Varchar") {
        value_t v{std::string_view{"hello"}};
        CHECK(v.type() == type::id_t::VARCHAR);
        CHECK(!v.is_null());
        CHECK(v.get_value().as<std::string_view>() == "hello");
    }

    SECTION("Make Null") {
        value_t v{value_t::make_null(type::id_t::INTEGER)};
        CHECK(v.type() == type::id_t::INTEGER);
        CHECK(v.is_null());
        CHECK(v.nullable());
    }
}

} // namespace cairn::tests
