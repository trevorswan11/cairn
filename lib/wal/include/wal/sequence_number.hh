#pragma once

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::wal {

// Log sequence number
enum class lsn_t : i64 {};
constexpr lsn_t INVALID_LSN{-1};

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

} // namespace stdx
