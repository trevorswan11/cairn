#pragma once

#include <algorithm>

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::sql::type {

enum class id_t : u8 {
    BOOLEAN = 1,
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

[[nodiscard]] constexpr auto is_numeric(stdx::option<id_t> type) noexcept -> bool {
    return type
        .transform([](id_t t) {
            return t == id_t::TINYINT || t == id_t::SMALLINT || t == id_t::INTEGER ||
                   t == id_t::BIGINT || t == id_t::FLOAT || t == id_t::DOUBLE;
        })
        .value_or(false);
}

[[nodiscard]] constexpr auto get_size_of(stdx::option<id_t> type) noexcept -> stdx::option<u32> {
    if (!type) { return stdx::none; }
    switch (*type) {
    case id_t::BOOLEAN:  return 1;
    case id_t::TINYINT:  return 1;
    case id_t::SMALLINT: return 2;
    case id_t::INTEGER:  return 4;
    case id_t::BIGINT:   return 8;
    case id_t::FLOAT:    return 4;
    case id_t::DOUBLE:   return 8;
    case id_t::DATETIME: return sizeof(datetime_t);
    case id_t::VARCHAR:  return stdx::none;
    }
    return stdx::none;
}

[[nodiscard]] constexpr auto numeric_rank(stdx::option<id_t> type) noexcept -> u32 {
    return type
        .transform([](id_t t) -> u32 {
            switch (t) {
            case id_t::TINYINT:  return 1;
            case id_t::SMALLINT: return 2;
            case id_t::INTEGER:  return 3;
            case id_t::BIGINT:   return 4;
            case id_t::FLOAT:    return 5;
            case id_t::DOUBLE:   return 6;
            case id_t::BOOLEAN:
            case id_t::VARCHAR:
            case id_t::DATETIME: return 0;
            }
        })
        .value_or(0);
}

[[nodiscard]] constexpr auto can_coerce(stdx::option<id_t> from, stdx::option<id_t> to) noexcept
    -> bool {
    if (!from || !to) { return false; }
    if (*from == *to) { return true; }
    if (is_numeric(from) && is_numeric(to)) { return numeric_rank(from) <= numeric_rank(to); }
    return false;
}

[[nodiscard]] constexpr auto common_type(stdx::option<id_t> a, stdx::option<id_t> b) noexcept
    -> stdx::option<id_t> {
    if (!a || !b) { return stdx::none; }
    if (*a == *b) { return a; }
    if (is_numeric(a) && is_numeric(b)) {
        switch (std::max(numeric_rank(a), numeric_rank(b))) {
        case 1:  return id_t::TINYINT;
        case 2:  return id_t::SMALLINT;
        case 3:  return id_t::INTEGER;
        case 4:  return id_t::BIGINT;
        case 5:  return id_t::FLOAT;
        case 6:  return id_t::DOUBLE;
        default: return stdx::none;
        }
    }
    return stdx::none;
}

} // namespace cairn::sql::type
