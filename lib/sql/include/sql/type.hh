#pragma once

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::sql::type {

enum class id_t : u8 {
    INVALID = 0,
    BOOLEAN,
    TINYINT,
    SMALLINT,
    INTEGER,
    BIGINT,
    FLOAT,
    DOUBLE,
    VARCHAR,
    DATETIME,
};

class datetime_t {
  public:
    using underlying_type_t = i64;

  public:
    constexpr explicit datetime_t(underlying_type_t raw) noexcept : raw_{raw} {}

    [[nodiscard]] constexpr auto operator==(const datetime_t&) const noexcept -> bool = default;

  private:
    underlying_type_t raw_;

    friend class tuple;
};

[[nodiscard]] constexpr auto is_numeric(type::id_t t) noexcept -> bool {
    return t == type::id_t::TINYINT || t == type::id_t::SMALLINT || t == type::id_t::INTEGER ||
           t == type::id_t::BIGINT || t == type::id_t::FLOAT || t == type::id_t::DOUBLE;
}

[[nodiscard]] constexpr auto get_size_of(id_t type) noexcept -> stdx::option<u32> {
    switch (type) {
    case id_t::BOOLEAN:  return 1;
    case id_t::TINYINT:  return 1;
    case id_t::SMALLINT: return 2;
    case id_t::INTEGER:  return 4;
    case id_t::BIGINT:   return 8;
    case id_t::FLOAT:    return 4;
    case id_t::DOUBLE:   return 8;
    case id_t::DATETIME: return sizeof(datetime_t);
    case id_t::VARCHAR:  return stdx::none;
    case id_t::INVALID:  return stdx::none;
    }
    return 0;
}

[[nodiscard]] constexpr auto numeric_rank(id_t t) noexcept -> u32 {
    switch (t) {
    case id_t::TINYINT:  return 1;
    case id_t::SMALLINT: return 2;
    case id_t::INTEGER:  return 3;
    case id_t::BIGINT:   return 4;
    case id_t::FLOAT:    return 5;
    case id_t::DOUBLE:   return 6;
    default:             return 0;
    }
}

[[nodiscard]] constexpr auto can_coerce(id_t from, id_t to) noexcept -> bool {
    if (from == id_t::INVALID || to == id_t::INVALID) { return false; }
    if (from == to) { return true; }
    if (is_numeric(from) && is_numeric(to)) {
        return numeric_rank(from) <= numeric_rank(to);
    }
    return false;
}

[[nodiscard]] constexpr auto common_type(id_t a, id_t b) noexcept -> stdx::option<id_t> {
    if (a == id_t::INVALID || b == id_t::INVALID) { return stdx::none; }
    if (a == b) { return a; }
    if (is_numeric(a) && is_numeric(b)) {
        const u32 rank_a = numeric_rank(a);
        const u32 rank_b = numeric_rank(b);
        const u32 max_rank = (rank_a > rank_b) ? rank_a : rank_b;
        switch (max_rank) {
        case 1: return id_t::TINYINT;
        case 2: return id_t::SMALLINT;
        case 3: return id_t::INTEGER;
        case 4: return id_t::BIGINT;
        case 5: return id_t::FLOAT;
        case 6: return id_t::DOUBLE;
        default: return stdx::none;
        }
    }
    return stdx::none;
}

} // namespace cairn::sql::type
