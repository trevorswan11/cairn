#pragma once

#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::txn {

enum class id_t : i64 {};
constexpr id_t INVALID_TXN_ID{-1};
using id_hash_t = stdx::hash<txn::id_t>;

enum class timestamp_t : u64 {};
constexpr timestamp_t INVALID_TIMESTAMP{stdx::nullable<timestamp_t>::invalid()};

enum class isolation_level_t : u8 {
    SNAPSHOT,
    SERIALIZABLE,
};

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
