#pragma once

#include <cstddef>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <utility>
#include <vector>

#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/error.hh"

namespace cairn::wal {

enum class lsn_t : i64 {};
constexpr lsn_t INVALID_LSN{-1};

[[nodiscard]] constexpr auto operator+(lsn_t lhs, lsn_t rhs) -> lsn_t {
    return lsn_t{std::to_underlying(lhs) + std::to_underlying(rhs)};
}

enum class txn_id_t : i64 {};
constexpr txn_id_t INVALID_TXN_ID{-1};

} // namespace cairn::wal

namespace stdx {

template <> struct nullable<cairn::wal::lsn_t> {
    using lsn_t = cairn::wal::lsn_t;
    [[nodiscard]] static constexpr auto invalid() noexcept -> lsn_t {
        return cairn::wal::INVALID_LSN;
    }

    [[nodiscard]] static constexpr auto is_valid(const lsn_t& id) noexcept -> bool {
        return id != invalid();
    }
};

template <> struct nullable<cairn::wal::txn_id_t> {
    using tid_t = cairn::wal::txn_id_t;
    [[nodiscard]] static constexpr auto invalid() noexcept -> tid_t {
        return cairn::wal::INVALID_TXN_ID;
    }

    [[nodiscard]] static constexpr auto is_valid(const tid_t& id) noexcept -> bool {
        return id != invalid();
    }
};

} // namespace stdx

namespace cairn::wal {

enum class log_record_type : u8 {
    BEGIN,
    COMMIT,
    ABORT,
    UPDATE,
    CLEAR,
    CHECKPOINT_BEGIN,
    CHECKPOINT_END,
};

struct log_record {
    i32                 size{0};
    lsn_t               lsn{INVALID_LSN};
    stdx::option<lsn_t> prev_lsn;
    txn_id_t            txn_id{INVALID_TXN_ID};
    log_record_type     type{log_record_type::BEGIN};

    stdx::option<storage::page_id_t> page_id;
    stdx::option<storage::slot_id_t> slot_id;
    stdx::option<lsn_t>              undo_next_lsn;

    gsl::span<const std::byte> redo_data;
    gsl::span<const std::byte> undo_data;

    u32 checksum{0};

    template <stdx::NumericIntegral I = usize>
    static constexpr auto MINIMUM_SIZE{static_cast<I>(
        sizeof(log_record::size) + sizeof(log_record::lsn) + sizeof(log_record::prev_lsn) +
        sizeof(log_record::txn_id) + sizeof(log_record::type) + sizeof(log_record::checksum) +
        sizeof(log_record::size))};

    template <stdx::NumericIntegral I = usize> [[nodiscard]] auto get_size() const noexcept -> I {
        return static_cast<I>(size);
    }

    // This is infallible for all intents and purposes assuming you have enough memory
    auto serialize(std::vector<std::byte>& dest) const -> void;

    // The provided span is advanced to the next record only on success
    [[nodiscard]] static auto deserialize(gsl::span<const std::byte>& src) noexcept
        -> result<log_record>;
};

} // namespace cairn::wal
