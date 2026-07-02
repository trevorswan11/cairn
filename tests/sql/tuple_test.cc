#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/fixed/vector.hh>
#include <stdx/types.hh>

#include "sql/schema.hh"
#include "sql/tuple.hh"
#include "sql/type.hh"
#include "sql/value.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::sql;

TEST_CASE("tuple serialization and deserialization") {
    std::vector<column> cols{{"id", type::id_t::INTEGER},
                             {"name", type::id_t::VARCHAR},
                             {"is_active", type::id_t::BOOLEAN},
                             {"score", type::id_t::DOUBLE},
                             {"description", type::id_t::VARCHAR, true}};
    schema              sch{cols};

    stdx::fixed::vector<value, 5> out_vals;
    out_vals.resize(out_vals.capacity());

    SECTION("Serialize and Deserialize basic row") {
        std::vector<value> vals{value{i32{42}},
                                value{std::string_view{"Alice"}},
                                value{true},
                                value{double{99.5}},
                                value::make_null(type::id_t::VARCHAR)};
        const auto         t{helpers::unwrap(tuple::serialize(sch, vals))};

        REQUIRE(t.deserialize(sch, out_vals));
        REQUIRE(out_vals.size() == 5);

        CHECK(out_vals[0].get_value().as<i32>() == 42);
        CHECK(out_vals[1].get_value().as<std::string_view>() == "Alice");
        CHECK(out_vals[2].get_value().as<bool>() == true);
        CHECK(out_vals[3].get_value().as<double>() == 99.5);
        CHECK(out_vals[4].is_null());
    }

    SECTION("All nulls") {
        const std::vector<value> vals{value::make_null(type::id_t::INTEGER),
                                      value::make_null(type::id_t::VARCHAR),
                                      value::make_null(type::id_t::BOOLEAN),
                                      value::make_null(type::id_t::DOUBLE),
                                      value::make_null(type::id_t::VARCHAR)};
        const auto               t{helpers::unwrap(tuple::serialize(sch, vals))};

        REQUIRE(t.deserialize(sch, out_vals));
        REQUIRE(out_vals.size() == 5);
        for (const auto& v : out_vals) { CHECK(v.is_null()); }
    }
}

} // namespace cairn::tests
