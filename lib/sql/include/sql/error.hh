#pragma once

#include <stdx/result.hh>
#include <stdx/types.hh>

namespace cairn::sql {

enum class error_t : u8 {
    VALUE_SCHEMA_COUNT_MISMATCH, // the value count did not match the schema count
    INVALID_COLUMN_INDEX,        // attempt to get value not in schema column range
    UNSUPPORTED_FIXED_TYPE,      // attempt to get a type unknown to the tuple deserializer
};

template <typename T> using result = stdx::result<T, error_t>;

} // namespace cairn::sql
