#include "helpers/mock_records.hh"

#include <algorithm>
#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/option.hh>

#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "wal/log_record.hh"

namespace cairn::tests::helpers {

auto write_begin_log(std::vector<std::byte>& buffer) -> wal::log_record {
    wal::log_record rec;
    rec.size     = 37;
    rec.lsn      = wal::lsn_t{42};
    rec.prev_lsn = wal::lsn_t{10};
    rec.txn_id   = txn::id_t{3};
    rec.type     = wal::log_record_type::BEGIN;
    rec.checksum = 297'829'837;

    rec.serialize(buffer);
    return rec;
}

auto write_update_log(std::vector<std::byte>& buffer) -> wal::log_record {
    wal::log_record rec;
    rec.size      = 62;
    rec.lsn       = wal::lsn_t{43};
    rec.prev_lsn  = wal::lsn_t{42};
    rec.txn_id    = txn::id_t{3};
    rec.type      = wal::log_record_type::UPDATE;
    rec.page_id   = storage::page_id_t{101};
    rec.slot_id   = storage::slot_id_t{5};
    rec.redo_data = redo_bytes;
    rec.undo_data = undo_bytes;
    rec.checksum  = 1'277'372'697;

    rec.serialize(buffer);
    return rec;
}

auto write_clear_log(std::vector<std::byte>& buffer) -> wal::log_record {
    wal::log_record rec;
    rec.size          = 64;
    rec.lsn           = wal::lsn_t{44};
    rec.prev_lsn      = stdx::none;
    rec.txn_id        = txn::id_t{3};
    rec.type          = wal::log_record_type::CLEAR;
    rec.page_id       = storage::page_id_t{102};
    rec.slot_id       = stdx::none;
    rec.undo_next_lsn = wal::lsn_t{10};
    rec.redo_data     = redo_bytes;
    rec.checksum      = 2'118'965'006;

    rec.serialize(buffer);
    return rec;
}

auto read_check_log(const wal::log_record& original, gsl::span<const std::byte>& src)
    -> wal::log_record {
    const auto decoded{helpers::unwrap(wal::log_record::deserialize(src))};
    records_eq(original, decoded);
    return decoded;
}

auto records_eq(const wal::log_record& a, const wal::log_record& b) noexcept -> void {
    CHECK(a.size == b.size);
    CHECK(a.lsn == b.lsn);
    CHECK(a.prev_lsn == b.prev_lsn);
    CHECK(a.txn_id == b.txn_id);
    CHECK(a.type == b.type);
    CHECK(a.page_id == b.page_id);
    CHECK(a.slot_id == b.slot_id);
    CHECK(a.undo_next_lsn == b.undo_next_lsn);
    CHECK(std::ranges::equal(a.redo_data, b.redo_data));
    CHECK(std::ranges::equal(a.undo_data, b.undo_data));
    REQUIRE(a.checksum == b.checksum);
}

} // namespace cairn::tests::helpers
