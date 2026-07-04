#pragma once

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::txn {

enum class id_t : i64 {};
constexpr id_t INVALID_TXN_ID{-1};

} // namespace cairn::txn

namespace stdx {

template <> struct nullable<cairn::txn::id_t> {
    using tid_t = cairn::txn::id_t;
    [[nodiscard]] static constexpr auto invalid() noexcept -> tid_t {
        return cairn::txn::INVALID_TXN_ID;
    }

    [[nodiscard]] static constexpr auto is_valid(const tid_t& id) noexcept -> bool {
        return id != invalid();
    }
};

} // namespace stdx
