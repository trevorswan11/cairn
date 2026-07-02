#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include <gsl/span>
#include <stdx/types.hh>

#include "sql/error.hh"
#include "sql/schema.hh"
#include "sql/value.hh"

namespace cairn::sql {

class tuple {
  public:
    tuple() = default;
    explicit tuple(std::vector<std::byte> data) : data_{std::move(data)} {}

    [[nodiscard]] auto data() const noexcept -> gsl::span<const std::byte> { return data_; }
    [[nodiscard]] auto size() const noexcept -> usize { return data_.size(); }

    [[nodiscard]] auto get_value(const schema& sch, usize column_index) const -> result<value>;

    // Serializes a list of values into a tuple byte array according to the schema
    static auto serialize(const std::vector<value>& values, const schema& sch) -> result<tuple>;

    // Deserializes the whole tuple into a list of values
    [[nodiscard]] auto deserialize(const schema& sch) const -> result<std::vector<value>>;

  private:
    std::vector<std::byte> data_;
};

} // namespace cairn::sql
