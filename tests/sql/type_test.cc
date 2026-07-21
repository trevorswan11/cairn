#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>

#include "sql/type.hh"

namespace cairn::tests {

using namespace cairn::sql;

TEST_CASE("sql::type coercion and common_type") {
    SECTION("can_coerce identity and invalid") {
        CHECK(type::can_coerce(type::id_t::INTEGER, type::id_t::INTEGER));
        CHECK(type::can_coerce(type::id_t::VARCHAR, type::id_t::VARCHAR));
        CHECK_FALSE(type::can_coerce(stdx::none, type::id_t::INTEGER));
        CHECK_FALSE(type::can_coerce(type::id_t::INTEGER, stdx::none));
    }

    SECTION("can_coerce numeric hierarchy") {
        CHECK(type::can_coerce(type::id_t::TINYINT, type::id_t::BIGINT));
        CHECK(type::can_coerce(type::id_t::INTEGER, type::id_t::DOUBLE));
        CHECK(type::can_coerce(type::id_t::FLOAT, type::id_t::DOUBLE));

        CHECK_FALSE(type::can_coerce(type::id_t::DOUBLE, type::id_t::INTEGER));
        CHECK_FALSE(type::can_coerce(type::id_t::BIGINT, type::id_t::SMALLINT));
    }

    SECTION("can_coerce non-numeric type mismatches") {
        CHECK_FALSE(type::can_coerce(type::id_t::VARCHAR, type::id_t::INTEGER));
        CHECK_FALSE(type::can_coerce(type::id_t::BOOLEAN, type::id_t::INTEGER));
        CHECK_FALSE(type::can_coerce(type::id_t::DATETIME, type::id_t::BIGINT));
    }

    SECTION("common_type resolution") {
        CHECK(type::common_type(type::id_t::INTEGER, type::id_t::INTEGER) == type::id_t::INTEGER);
        CHECK(type::common_type(type::id_t::INTEGER, type::id_t::DOUBLE) == type::id_t::DOUBLE);
        CHECK(type::common_type(type::id_t::FLOAT, type::id_t::BIGINT) == type::id_t::FLOAT);
        CHECK(type::common_type(type::id_t::SMALLINT, type::id_t::TINYINT) == type::id_t::SMALLINT);

        CHECK_FALSE(type::common_type(type::id_t::VARCHAR, type::id_t::INTEGER));
        CHECK_FALSE(type::common_type(type::id_t::BOOLEAN, type::id_t::DOUBLE));
        CHECK_FALSE(type::common_type(stdx::none, type::id_t::INTEGER));
    }
}

} // namespace cairn::tests
