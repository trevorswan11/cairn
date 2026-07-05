#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>

#include "helpers/mock_records.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "wal/log/reader.hh"

namespace cairn::tests {

using namespace cairn::wal;

TEST_CASE("log::reader bidirectional scan") {
    helpers::tempfile      file{"wal_reader"};
    std::vector<std::byte> buffer;

    // Serialize 3 records
    const auto rec1{helpers::write_begin_log(buffer)};
    const auto rec2{helpers::write_update_log(buffer)};
    const auto rec3{helpers::write_clear_log(buffer)};

    {
        std::ofstream out{file.path, std::ios::out | std::ios::binary};
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size()));
        REQUIRE_FALSE(out.fail());
    }

    using helpers::unwrap;
    auto reader{unwrap(log::reader::open(file.path))};

    SECTION("Sequential bidirectional access") {
        helpers::records_eq(unwrap(unwrap(reader.next_record())), rec1);
        helpers::records_eq(unwrap(unwrap(reader.next_record())), rec2);
        helpers::records_eq(unwrap(unwrap(reader.next_record())), rec3);
        CHECK_FALSE(unwrap(reader.next_record()).has_value());

        helpers::records_eq(unwrap(unwrap(reader.prev_record())), rec3);
        helpers::records_eq(unwrap(unwrap(reader.prev_record())), rec2);
        helpers::records_eq(unwrap(unwrap(reader.prev_record())), rec1);
        CHECK_FALSE(unwrap(reader.prev_record()).has_value());
    }

    SECTION("Alternating access") {
        helpers::records_eq(unwrap(unwrap(reader.next_record())), rec1);
        helpers::records_eq(unwrap(unwrap(reader.prev_record())), rec1);
        CHECK_FALSE(unwrap(reader.prev_record()).has_value());

        helpers::records_eq(unwrap(unwrap(reader.next_record())), rec1);
        helpers::records_eq(unwrap(unwrap(reader.next_record())), rec2);
        helpers::records_eq(unwrap(unwrap(reader.next_record())), rec3);
        helpers::records_eq(unwrap(unwrap(reader.prev_record())), rec3);
        helpers::records_eq(unwrap(unwrap(reader.next_record())), rec3);
        CHECK_FALSE(unwrap(reader.next_record()).has_value());
    }

    SECTION("Low-level next_at and prev_at APIs") {
        auto next1{unwrap(reader.has_next())};
        helpers::records_eq(unwrap(reader.next_at(unwrap(next1))), rec1);
        auto next2{unwrap(reader.has_next())};
        helpers::records_eq(unwrap(reader.next_at(unwrap(next2))), rec2);
        auto next3{unwrap(reader.has_next())};
        helpers::records_eq(unwrap(reader.next_at(unwrap(next3))), rec3);
        auto prev3{unwrap(reader.has_prev())};
        helpers::records_eq(unwrap(reader.prev_at(unwrap(prev3))), rec3);
    }

    SECTION("High-level next_record, next_record_lenient, and prev_record APIs") {
        auto next1{unwrap(reader.next_record())};
        helpers::records_eq(unwrap(next1), rec1);
        auto next2{unwrap(reader.next_record_lenient())};
        helpers::records_eq(unwrap(next2), rec2);
        auto next3{unwrap(reader.next_record())};
        helpers::records_eq(unwrap(next3), rec3);

        CHECK_FALSE(unwrap(reader.next_record()));
        CHECK_FALSE(unwrap(reader.next_record_lenient()));

        auto prev3{unwrap(reader.prev_record())};
        helpers::records_eq(unwrap(prev3), rec3);
        auto prev2{unwrap(reader.prev_record())};
        helpers::records_eq(unwrap(prev2), rec2);
        auto prev1{unwrap(reader.prev_record())};
        helpers::records_eq(unwrap(prev1), rec1);

        CHECK_FALSE(unwrap(reader.prev_record()));
    }
}

} // namespace cairn::tests
