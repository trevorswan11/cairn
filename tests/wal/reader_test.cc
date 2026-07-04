#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>

#include "helpers/mock_records.hh"
#include "support/error.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "wal/reader.hh"

namespace cairn::tests {

using namespace cairn::wal;

TEST_CASE("log_reader bidirectional scan") {
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
    auto reader{helpers::unwrap(reader::open(file.path))};

    SECTION("Sequential bidirectional access") {
        helpers::records_eq(helpers::unwrap(reader.next()), rec1);
        helpers::records_eq(helpers::unwrap(reader.next()), rec2);
        helpers::records_eq(helpers::unwrap(reader.next()), rec3);
        CHECK(helpers::unwrap_err(reader.next()) == error_t::WAL_EOF);

        helpers::records_eq(helpers::unwrap(reader.prev()), rec3);
        helpers::records_eq(helpers::unwrap(reader.prev()), rec2);
        helpers::records_eq(helpers::unwrap(reader.prev()), rec1);
        CHECK(helpers::unwrap_err(reader.prev()) == error_t::WAL_EOF);
    }

    SECTION("Alternating access") {
        helpers::records_eq(helpers::unwrap(reader.next()), rec1);
        helpers::records_eq(helpers::unwrap(reader.prev()), rec1);
        CHECK(helpers::unwrap_err(reader.prev()) == error_t::WAL_EOF);

        helpers::records_eq(helpers::unwrap(reader.next()), rec1);
        helpers::records_eq(helpers::unwrap(reader.next()), rec2);
        helpers::records_eq(helpers::unwrap(reader.next()), rec3);
        helpers::records_eq(helpers::unwrap(reader.prev()), rec3);
        helpers::records_eq(helpers::unwrap(reader.next()), rec3);
        CHECK(helpers::unwrap_err(reader.next()) == error_t::WAL_EOF);
    }
}

} // namespace cairn::tests
