#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>

#include "helpers/mock_records.hh"
#include "support/diagnostic/error.hh"
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

    auto reader{UNWRAP(log::reader::open(file.path))};

    SECTION("Sequential bidirectional access") {
        helpers::records_eq(UNWRAP(UNWRAP(reader.next_record())), rec1);
        helpers::records_eq(UNWRAP(UNWRAP(reader.next_record())), rec2);
        helpers::records_eq(UNWRAP(UNWRAP(reader.next_record())), rec3);
        CHECK_FALSE(UNWRAP(reader.next_record()).has_value());

        helpers::records_eq(UNWRAP(UNWRAP(reader.prev_record())), rec3);
        helpers::records_eq(UNWRAP(UNWRAP(reader.prev_record())), rec2);
        helpers::records_eq(UNWRAP(UNWRAP(reader.prev_record())), rec1);
        CHECK_FALSE(UNWRAP(reader.prev_record()).has_value());
    }

    SECTION("Alternating access") {
        helpers::records_eq(UNWRAP(UNWRAP(reader.next_record())), rec1);
        helpers::records_eq(UNWRAP(UNWRAP(reader.prev_record())), rec1);
        CHECK_FALSE(UNWRAP(reader.prev_record()).has_value());

        helpers::records_eq(UNWRAP(UNWRAP(reader.next_record())), rec1);
        helpers::records_eq(UNWRAP(UNWRAP(reader.next_record())), rec2);
        helpers::records_eq(UNWRAP(UNWRAP(reader.next_record())), rec3);
        helpers::records_eq(UNWRAP(UNWRAP(reader.prev_record())), rec3);
        helpers::records_eq(UNWRAP(UNWRAP(reader.next_record())), rec3);
        CHECK_FALSE(UNWRAP(reader.next_record()).has_value());
    }

    SECTION("Low-level next_at and prev_at APIs") {
        auto next1{UNWRAP(reader.has_next())};
        helpers::records_eq(UNWRAP(reader.next_at(UNWRAP(next1))), rec1);
        auto next2{UNWRAP(reader.has_next())};
        helpers::records_eq(UNWRAP(reader.next_at(UNWRAP(next2))), rec2);
        auto next3{UNWRAP(reader.has_next())};
        helpers::records_eq(UNWRAP(reader.next_at(UNWRAP(next3))), rec3);
        auto prev3{UNWRAP(reader.has_prev())};
        helpers::records_eq(UNWRAP(reader.prev_at(UNWRAP(prev3))), rec3);
    }

    SECTION("High-level next_record, next_record_lenient, and prev_record APIs") {
        auto next1{UNWRAP(reader.next_record())};
        helpers::records_eq(UNWRAP(next1), rec1);
        auto next2{UNWRAP(reader.next_record_lenient())};
        helpers::records_eq(UNWRAP(next2), rec2);
        auto next3{UNWRAP(reader.next_record())};
        helpers::records_eq(UNWRAP(next3), rec3);

        CHECK_FALSE(UNWRAP(reader.next_record()));
        CHECK_FALSE(UNWRAP(reader.next_record_lenient()));

        auto prev3{UNWRAP(reader.prev_record())};
        helpers::records_eq(UNWRAP(prev3), rec3);
        auto prev2{UNWRAP(reader.prev_record())};
        helpers::records_eq(UNWRAP(prev2), rec2);
        auto prev1{UNWRAP(reader.prev_record())};
        helpers::records_eq(UNWRAP(prev1), rec1);

        CHECK_FALSE(UNWRAP(reader.prev_record()));
    }
}

TEST_CASE("log::reader lenient reading with size corruption") {
    helpers::tempfile      corrupt_file{"wal_reader_corrupt"};
    std::vector<std::byte> corrupt_buffer;

    // Serialize 1 good record and 2 bytes of garbage
    const auto corrupt_rec1{helpers::write_begin_log(corrupt_buffer)};
    corrupt_buffer.emplace_back(std::byte{0x01});
    corrupt_buffer.emplace_back(std::byte{0x02});

    {
        std::ofstream out{corrupt_file.path, std::ios::out | std::ios::binary};
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(corrupt_buffer.data()),
                  static_cast<std::streamsize>(corrupt_buffer.size()));
        REQUIRE_FALSE(out.fail());
    }

    auto reader_corrupt{UNWRAP(log::reader::open(corrupt_file.path))};
    helpers::records_eq(UNWRAP(UNWRAP(reader_corrupt.next_record())), corrupt_rec1);

    CHECK(UNWRAP_ERR(reader_corrupt.next_record()) == error::WAL_SIZE_CORRUPT);
    auto reader_lenient{UNWRAP(log::reader::open(corrupt_file.path))};
    helpers::records_eq(UNWRAP(UNWRAP(reader_lenient.next_record_lenient())), corrupt_rec1);

    CHECK_FALSE(UNWRAP(reader_lenient.next_record_lenient()));
}

} // namespace cairn::tests
