#pragma once

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::wal::log {

// Log sequence number
enum class seq_num : i64 {};
constexpr seq_num INVALID_LSN{-1};

} // namespace cairn::wal::log

namespace stdx {

template <> struct nullable<cairn::wal::log::seq_num> {
    using lsn_t = cairn::wal::log::seq_num;
    [[nodiscard]] static constexpr auto invalid() noexcept -> lsn_t {
        return cairn::wal::log::INVALID_LSN;
    }

    [[nodiscard]] static constexpr auto is_valid(const lsn_t& id) noexcept -> bool {
        return id != invalid();
    }
};

} // namespace stdx
