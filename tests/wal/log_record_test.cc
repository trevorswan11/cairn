#include <cstddef>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/option.hh>

#include "error.hh"
#include "helpers/mock_records.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "testhelpers/unwrap.hh"
#include "wal/log_record.hh"

namespace cairn::tests {

using namespace cairn::wal;

TEST_CASE("log_record begin") {
    std::vector<std::byte>     buffer;
    auto                       rec{helpers::write_begin_log(buffer)};
    gsl::span<const std::byte> src{buffer};
    helpers::read_check_log(rec, src);
    CHECK(src.empty());
}

TEST_CASE("log_record update") {
    std::vector<std::byte>     buffer;
    auto                       rec{helpers::write_update_log(buffer)};
    gsl::span<const std::byte> src{buffer};
    helpers::read_check_log(rec, src);
    CHECK(src.empty());
}

TEST_CASE("log_record clear") {
    std::vector<std::byte>     buffer;
    auto                       rec{helpers::write_clear_log(buffer)};
    gsl::span<const std::byte> src{buffer};
    helpers::read_check_log(rec, src);
    CHECK(src.empty());
}

TEST_CASE("log_record multiple read/write") {
    std::vector<std::byte> buffer;
    auto                   begin_rec{helpers::write_begin_log(buffer)};
    auto                   update_rec{helpers::write_update_log(buffer)};
    auto                   clear_rec{helpers::write_clear_log(buffer)};

    gsl::span<const std::byte> src{buffer};
    helpers::read_check_log(begin_rec, src);
    helpers::read_check_log(update_rec, src);
    helpers::read_check_log(clear_rec, src);
    CHECK(src.empty());
}

TEST_CASE("log_record checksum corruption") {
    log_record rec;
    rec.lsn       = lsn_t{45};
    rec.txn_id    = txn_id_t{3};
    rec.type      = log_record_type::UPDATE;
    rec.page_id   = storage::page_id_t{101};
    rec.slot_id   = storage::slot_id_t{1};
    rec.redo_data = helpers::redo_bytes;

    std::vector<std::byte> buffer;
    rec.serialize(buffer);

    // Corrupt one byte of the payload
    buffer[25] ^= std::byte{0xFF};

    gsl::span<const std::byte> src{buffer};
    CHECK(helpers::unwrap_err(log_record::deserialize(src)) == error_t::WAL_CHECKSUM_CORRUPT);
}

} // namespace cairn::tests
