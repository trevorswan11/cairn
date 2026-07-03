#pragma once

#include <cstddef>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <vector>

#include "error.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"

namespace cairn::wal {

enum class lsn_t : i64 {};
constexpr lsn_t INVALID_LSN{-1};

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

    storage::page_id_t  page_id{storage::INVALID_PAGE_ID};
    storage::slot_id_t  slot_id{storage::INVALID_SLOT_ID};
    stdx::option<lsn_t> undo_next_lsn;

    std::vector<std::byte> redo_data;
    std::vector<std::byte> undo_data;

    u32 checksum{0};

    template <stdx::NumericIntegral I = usize> [[nodiscard]] auto get_size() const noexcept -> I {
        return static_cast<I>(size);
    }

    [[nodiscard]] auto        serialize(std::vector<std::byte>& dest) const -> result<void>;
    [[nodiscard]] static auto deserialize(gsl::span<const std::byte> src) -> result<log_record>;
};

} // namespace cairn::wal
