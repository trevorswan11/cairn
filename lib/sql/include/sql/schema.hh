#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include <gsl/span>
#include <stdx/fixed/string.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "sql/type.hh"

namespace cairn::sql {

class column {
  public:
    column(stdx::fixed::string name, type::id_t type, bool nullable = true)
        : name_{std::move(name)}, type_{type}, nullable_{nullable} {}

    [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }
    [[nodiscard]] auto type() const noexcept -> type::id_t { return type_; }
    [[nodiscard]] auto nullable() const noexcept -> bool { return nullable_; }
    [[nodiscard]] auto size() const noexcept -> stdx::option<u32> {
        return type::get_size_of(type_);
    }

  private:
    stdx::fixed::string name_;
    type::id_t          type_;
    bool                nullable_;
};

class schema {
  public:
    explicit schema(std::vector<column> columns) : columns_{std::move(columns)} {
        for (const auto& column : columns_) {
            if (const auto fixed_size{column.size()}) {
                fixed_length_size_ += *fixed_size;
            } else {
                has_varlen_ = true;
            }
        }
    }

    [[nodiscard]] auto columns() const noexcept -> gsl::span<const column> { return columns_; }
    [[nodiscard]] auto operator[](usize index) const -> const column& { return columns_[index]; }
    [[nodiscard]] auto column_count() const noexcept -> usize { return columns_.size(); }

    // Total size of all fixed length fields combined
    [[nodiscard]] auto fixed_length_size() const noexcept -> u32 { return fixed_length_size_; }
    [[nodiscard]] auto has_varlen() const noexcept -> bool { return has_varlen_; }

  private:
    std::vector<column> columns_;
    u32                 fixed_length_size_{0};
    bool                has_varlen_{false};
};

} // namespace cairn::sql
