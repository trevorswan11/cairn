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
                             {"description", type::id_t::VARCHAR, true},
                             {"tiny_val", type::id_t::TINYINT},
                             {"small_val", type::id_t::SMALLINT},
                             {"big_val", type::id_t::BIGINT},
                             {"float_val", type::id_t::FLOAT}};
    schema              sch{cols};

    stdx::fixed::vector<value_t, 9> out_vals;
    out_vals.resize(out_vals.capacity());

    SECTION("Serialize and Deserialize basic row") {
        std::vector<value_t> vals{value_t{i32{42}},
                                  value_t{std::string_view{"Alice"}},
                                  value_t{true},
                                  value_t{double{99.5}},
                                  value_t::make_null(type::id_t::VARCHAR),
                                  value_t{i8{7}},
                                  value_t{i16{256}},
                                  value_t{i64{999'999'999}},
                                  value_t{f32{3.14f}}};
        const auto           t{UNWRAP(tuple::serialize(sch, vals))};

        REQUIRE(t.deserialize(sch, out_vals));
        REQUIRE(out_vals.size() == 9);

        CHECK(out_vals[0].get_value().as<i32>() == 42);
        CHECK(out_vals[1].get_value().as<std::string_view>() == "Alice");
        CHECK(out_vals[2].get_value().as<bool>() == true);
        CHECK(out_vals[3].get_value().as<double>() == 99.5);
        CHECK(out_vals[4].is_null());
        CHECK(out_vals[5].get_value().as<i8>() == 7);
        CHECK(out_vals[6].get_value().as<i16>() == 256);
        CHECK(out_vals[7].get_value().as<i64>() == 999'999'999);
        CHECK(out_vals[8].get_value().as<f32>() == 3.14f);
    }

    SECTION("All nulls") {
        const std::vector<value_t> vals{value_t::make_null(type::id_t::INTEGER),
                                        value_t::make_null(type::id_t::VARCHAR),
                                        value_t::make_null(type::id_t::BOOLEAN),
                                        value_t::make_null(type::id_t::DOUBLE),
                                        value_t::make_null(type::id_t::VARCHAR),
                                        value_t::make_null(type::id_t::TINYINT),
                                        value_t::make_null(type::id_t::SMALLINT),
                                        value_t::make_null(type::id_t::BIGINT),
                                        value_t::make_null(type::id_t::FLOAT)};
        const auto                 t{UNWRAP(tuple::serialize(sch, vals))};

        CHECK(t.deserialize(sch, out_vals));
        CHECK(out_vals.size() == 9);
        for (const auto& v : out_vals) { CHECK(v.is_null()); }
    }
}

} // namespace cairn::tests
