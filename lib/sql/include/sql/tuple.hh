#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include <gsl/span>
#include <stdx/fixed/vector.hh>
#include <stdx/types.hh>

#include "sql/error.hh"
#include "sql/schema.hh"
#include "sql/value.hh"
#include "storage/page.hh"

namespace cairn::sql {

class tuple {
  public:
    using byte_buffer = stdx::fixed::vector<std::byte, storage::DB_PAGE_SIZE>;

  public:
    tuple() = default;
    explicit tuple(byte_buffer data) : data_{std::move(data)} {}

    [[nodiscard]] auto data() const noexcept -> gsl::span<const std::byte> { return data_; }
    [[nodiscard]] auto size() const noexcept -> usize { return data_.size(); }

    [[nodiscard]] auto get_value(const schema& sch, usize column_index) const -> result<value>;

    // Serializes a list of values into a tuple byte array according to the schema
    static auto serialize(const schema& sch, gsl::span<const value> values) -> result<tuple>;

    // Deserializes the whole tuple into a list of values
    [[nodiscard]] auto deserialize(const schema& sch) const -> result<std::vector<value>>;

  private:
    byte_buffer data_;
};

} // namespace cairn::sql
