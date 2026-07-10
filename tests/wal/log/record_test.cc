#include <cstddef>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/utility.hh>

#include "helpers/mock_records.hh"
#include "support/error.hh"
#include "testhelpers/unwrap.hh"
#include "wal/log/record.hh"

namespace cairn::tests {

using namespace cairn::wal;

TEST_CASE("log::record begin") {
    std::vector<std::byte>     buffer;
    const auto                 rec{helpers::write_begin_log(buffer)};
    gsl::span<const std::byte> src{buffer};
    helpers::read_check_log(rec, src);
    CHECK(src.empty());
}

TEST_CASE("log::record update") {
    std::vector<std::byte>     buffer;
    const auto                 rec{helpers::write_update_log(buffer)};
    gsl::span<const std::byte> src{buffer};
    helpers::read_check_log(rec, src);
    CHECK(src.empty());
}

TEST_CASE("log::record clear") {
    std::vector<std::byte>     buffer;
    auto                       rec{helpers::write_clear_log(buffer)};
    gsl::span<const std::byte> src{buffer};
    helpers::read_check_log(rec, src);
    CHECK(src.empty());
}

TEST_CASE("log::record multiple read/write") {
    std::vector<std::byte> buffer;
    const auto             begin_rec{helpers::write_begin_log(buffer)};
    const auto             update_rec{helpers::write_update_log(buffer)};
    const auto             clear_rec{helpers::write_clear_log(buffer)};

    gsl::span<const std::byte> src{buffer};
    helpers::read_check_log(begin_rec, src);
    helpers::read_check_log(update_rec, src);
    helpers::read_check_log(clear_rec, src);
    CHECK(src.empty());
}

TEST_CASE("log::record checksum corruption") {
    std::vector<std::byte> buffer;
    DISCARD(helpers::write_begin_log(buffer));

    buffer[25] ^= std::byte{0xFF};
    gsl::span<const std::byte> src{buffer};
    CHECK(UNWRAP_ERR(log::record::deserialize(src)) == error_t::WAL_CHECKSUM_CORRUPT);
}

} // namespace cairn::tests
