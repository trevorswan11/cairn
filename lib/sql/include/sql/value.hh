#pragma once

#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "sql/type.hh"

namespace cairn::sql {

// The main value abstraction over SQL data types
class value_t {
  public:
    using variant_t =
        stdx::variant<stdx::monostate, bool, i8, i16, i32, i64, f32, f64, std::string_view>;

  public:
    value_t() noexcept = default;

    // Explicit constructor tracking type independently
    explicit value_t(variant_t value, bool nullable = true) noexcept
        : value_{std::move(value)}, type_{derive_type(value_)}, nullable_{nullable} {}

    // Factory to create strongly-typed NULL values
    [[nodiscard]] static auto make_null(type::id_t t) noexcept -> value_t {
        value_t value;
        value.type_ = t;
        value.value_.emplace<stdx::monostate>();
        value.nullable_ = true;
        return value;
    }

    [[nodiscard]] auto nullable() const noexcept -> bool { return nullable_; }
    [[nodiscard]] auto is_null() const noexcept -> bool { return value_.is<stdx::monostate>(); }
    MAKE_DEDUCING_GETTER(value)

    [[nodiscard]] auto type() const noexcept -> type::id_t { return type_; }
    [[nodiscard]] auto size() const noexcept -> stdx::option<u32> {
        return type::get_size_of(type());
    }

  private:
    static auto derive_type(const variant_t& val) noexcept -> type::id_t {
        return val.visit([](stdx::monostate) { return type::id_t::INVALID; },
                         [](bool) { return type::id_t::BOOLEAN; },
                         [](i8) { return type::id_t::TINYINT; },
                         [](i16) { return type::id_t::SMALLINT; },
                         [](i32) { return type::id_t::INTEGER; },
                         [](i64) { return type::id_t::BIGINT; },
                         [](f32) { return type::id_t::FLOAT; },
                         [](f64) { return type::id_t::DOUBLE; },
                         [](std::string_view) { return type::id_t::VARCHAR; });
    }

  private:
    variant_t  value_;
    type::id_t type_{type::id_t::INVALID};
    bool       nullable_{true};
};

} // namespace cairn::sql
