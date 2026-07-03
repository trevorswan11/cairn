#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/option.hh>

#include "error.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "testhelpers/unwrap.hh"
#include "wal/log_record.hh"

namespace cairn::tests {

using namespace cairn::wal;

namespace {

constexpr std::array redo_bytes{std::byte{0x1}, std::byte{0x2}, std::byte{0x3}};
constexpr std::array undo_bytes{std::byte{0x9}, std::byte{0x8}};

[[nodiscard]] auto write_begin_log(std::vector<std::byte>& buffer) -> log_record {
    log_record rec;
    rec.lsn      = lsn_t{42};
    rec.prev_lsn = lsn_t{10};
    rec.txn_id   = txn_id_t{3};
    rec.type     = log_record_type::BEGIN;

    rec.serialize(buffer);
    return rec;
}

auto read_begin_log(const log_record& original, gsl::span<const std::byte>& src) -> void {
    auto decoded{helpers::unwrap(log_record::deserialize(src))};
    CHECK(decoded.lsn == original.lsn);
    CHECK(decoded.prev_lsn == original.prev_lsn);
    CHECK(decoded.txn_id == original.txn_id);
    CHECK(decoded.type == original.type);
}

[[nodiscard]] auto write_update_log(std::vector<std::byte>& buffer) -> log_record {
    log_record rec;
    rec.lsn       = lsn_t{43};
    rec.prev_lsn  = lsn_t{42};
    rec.txn_id    = txn_id_t{3};
    rec.type      = log_record_type::UPDATE;
    rec.page_id   = storage::page_id_t{101};
    rec.slot_id   = storage::slot_id_t{5};
    rec.redo_data = redo_bytes;
    rec.undo_data = undo_bytes;

    rec.serialize(buffer);
    return rec;
}

auto read_update_log(const log_record& original, gsl::span<const std::byte>& src) -> void {
    auto decoded{helpers::unwrap(log_record::deserialize(src))};

    CHECK(decoded.lsn == original.lsn);
    CHECK(decoded.prev_lsn == original.prev_lsn);
    CHECK(decoded.txn_id == original.txn_id);
    CHECK(decoded.type == original.type);
    CHECK(decoded.page_id == original.page_id);
    CHECK(decoded.slot_id == original.slot_id);

    CHECK(std::ranges::equal(decoded.redo_data, original.redo_data));
    CHECK(std::ranges::equal(decoded.undo_data, original.undo_data));
}

[[nodiscard]] auto write_clear_log(std::vector<std::byte>& buffer) -> log_record {
    log_record rec;
    rec.lsn           = lsn_t{44};
    rec.prev_lsn      = stdx::none;
    rec.txn_id        = txn_id_t{3};
    rec.type          = log_record_type::CLEAR;
    rec.page_id       = storage::page_id_t{102};
    rec.slot_id       = stdx::none;
    rec.undo_next_lsn = lsn_t{10};
    rec.redo_data     = redo_bytes;

    rec.serialize(buffer);
    return rec;
}

auto read_clear_log(const log_record& original, gsl::span<const std::byte>& src) -> void {
    auto decoded{helpers::unwrap(log_record::deserialize(src))};

    CHECK(decoded.lsn == original.lsn);
    CHECK(decoded.prev_lsn == original.prev_lsn);
    CHECK(decoded.txn_id == original.txn_id);
    CHECK(decoded.type == original.type);
    CHECK(decoded.page_id == original.page_id);
    CHECK(decoded.slot_id == original.slot_id);
    CHECK(decoded.undo_next_lsn == original.undo_next_lsn);

    CHECK(std::ranges::equal(decoded.redo_data, original.redo_data));
}

} // namespace

TEST_CASE("log_record begin") {
    std::vector<std::byte>     buffer;
    auto                       rec{write_begin_log(buffer)};
    gsl::span<const std::byte> src{buffer};
    read_begin_log(rec, src);
    CHECK(src.empty());
}

TEST_CASE("log_record update") {
    std::vector<std::byte>     buffer;
    auto                       rec{write_update_log(buffer)};
    gsl::span<const std::byte> src{buffer};
    read_update_log(rec, src);
    CHECK(src.empty());
}

TEST_CASE("log_record clear") {
    std::vector<std::byte>     buffer;
    auto                       rec{write_clear_log(buffer)};
    gsl::span<const std::byte> src{buffer};
    read_clear_log(rec, src);
    CHECK(src.empty());
}

TEST_CASE("log_record multiple read/write") {
    std::vector<std::byte> buffer;
    auto                   begin_rec{write_begin_log(buffer)};
    auto                   update_rec{write_update_log(buffer)};
    auto                   clear_rec{write_clear_log(buffer)};

    gsl::span<const std::byte> src{buffer};
    read_begin_log(begin_rec, src);
    read_update_log(update_rec, src);
    read_clear_log(clear_rec, src);
    CHECK(src.empty());
}

TEST_CASE("log_record checksum corruption") {
    log_record rec;
    rec.lsn       = lsn_t{45};
    rec.txn_id    = txn_id_t{3};
    rec.type      = log_record_type::UPDATE;
    rec.page_id   = storage::page_id_t{101};
    rec.slot_id   = storage::slot_id_t{1};
    rec.redo_data = redo_bytes;

    std::vector<std::byte> buffer;
    rec.serialize(buffer);

    // Corrupt one byte of the payload
    buffer[25] ^= std::byte{0xFF};

    gsl::span<const std::byte> src{buffer};
    CHECK(helpers::unwrap_err(log_record::deserialize(src)) == error_t::WAL_CHECKSUM_CORRUPT);
}

} // namespace cairn::tests
