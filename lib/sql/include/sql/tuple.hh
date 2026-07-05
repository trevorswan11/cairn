#pragma once

#include <cstddef>
#include <utility>

#include <gsl/span>
#include <stdx/fixed/vector.hh>
#include <stdx/types.hh>

#include "sql/schema.hh"
#include "sql/value.hh"
#include "storage/page.hh"
#include "support/error.hh"

namespace cairn::sql {

class tuple {
  public:
    using byte_buffer = stdx::fixed::vector<std::byte, storage::DB_PAGE_SIZE>;

  public:
    tuple() = default;
    explicit tuple(byte_buffer data) : data_{std::move(data)} {}

    [[nodiscard]] auto data() const noexcept -> gsl::span<const std::byte> { return data_; }
    [[nodiscard]] auto size() const noexcept -> usize { return data_.size(); }

    [[nodiscard]] auto get_value(const schema& sch, usize column_index) const -> result<value_t>;

    // Serializes a list of values into a tuple byte array according to the schema
    static auto serialize(const schema& sch, gsl::span<const value_t> values) -> result<tuple>;

    // Deserializes the whole tuple into a list of values
    [[nodiscard]] auto deserialize(const schema& sch, gsl::span<value_t> buf) const -> result<void>;

  private:
    byte_buffer data_;
};

} // namespace cairn::sql
