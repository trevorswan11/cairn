#pragma once

#include <cstddef>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <vector>

#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "wal/sequence_number.hh"

namespace cairn::wal {

enum class record_type : u8 {
    BEGIN,
    COMMIT,
    ABORT,
    UPDATE,
    CLEAR,
    CHECKPOINT_BEGIN,
    CHECKPOINT_END,
};

struct record {
    i32                 size{0};
    lsn_t               lsn{INVALID_LSN};
    stdx::option<lsn_t> prev_lsn;
    txn::id_t       txn_id{txn::INVALID_TXN_ID};
    record_type     type{record_type::BEGIN};

    stdx::option<storage::page_id_t> page_id;
    stdx::option<storage::slot_id_t> slot_id;
    stdx::option<lsn_t>              undo_next_lsn;

    gsl::span<const std::byte> redo_data;
    gsl::span<const std::byte> undo_data;

    u32 checksum{0};

    template <stdx::NumericIntegral I = usize>
    static constexpr auto MINIMUM_SIZE{static_cast<I>(
        sizeof(record::size) + sizeof(record::lsn) + sizeof(record::prev_lsn) +
        sizeof(record::txn_id) + sizeof(record::type) + sizeof(record::checksum) +
        sizeof(record::size))};

    template <stdx::NumericIntegral I = usize> [[nodiscard]] auto get_size() const noexcept -> I {
        return static_cast<I>(size);
    }

    // This is infallible for all intents and purposes assuming you have enough memory
    auto serialize(std::vector<std::byte>& dest) const -> void;

    // The provided span is advanced to the next record only on success
    [[nodiscard]] static auto deserialize(gsl::span<const std::byte>& src) noexcept
        -> result<record>;
};

} // namespace cairn::wal
