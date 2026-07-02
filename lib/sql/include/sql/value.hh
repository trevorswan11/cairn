#pragma once

#include <string>

#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "sql/type.hh"

namespace cairn::sql {

// The main value abstraction over SQL data types
class value {
  public:
    using variant_t =
        stdx::variant<stdx::monostate, bool, i8, i16, i32, i64, f32, f64, std::string>;

  public:
    value() noexcept = default;
    explicit value(variant_t val, bool nullable = true) noexcept
        : value_{std::move(val)}, nullable_{nullable} {}

    [[nodiscard]] auto nullable() const noexcept -> bool { return nullable_; }
    [[nodiscard]] auto is_null() const noexcept -> bool { return value_.is<stdx::monostate>(); }
    MAKE_DEDUCING_GETTER(value)

    [[nodiscard]] auto type() const noexcept -> type::id_t {
        return value_.visit([](stdx::monostate) { return type::id_t::INVALID; },
                            [](bool) { return type::id_t::BOOLEAN; },
                            [](i8) { return type::id_t::TINYINT; },
                            [](i16) { return type::id_t::SMALLINT; },
                            [](i32) { return type::id_t::INTEGER; },
                            [](i64) { return type::id_t::BIGINT; },
                            [](f32) { return type::id_t::FLOAT; },
                            [](f64) { return type::id_t::DOUBLE; },
                            [](const std::string&) { return type::id_t::VARCHAR; });
    }

    [[nodiscard]] auto size() const noexcept -> stdx::option<u32> {
        return type::get_size_of(type());
    }

  private:
    variant_t value_;
    bool      nullable_;
};

} // namespace cairn::sql
