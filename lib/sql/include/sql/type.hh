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

} // namespace cairn::sql::type
