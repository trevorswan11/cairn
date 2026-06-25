#pragma once

#include <stdx/result.hh>
#include <stdx/types.hh>

namespace cairn::support {

enum class error_t : u8 {
    JSON_PARSE_ERROR,
    JSON_TYPE_ERROR,
    JSON_MISSING_FIELD,
};

template <typename T> using result = stdx::result<T, error_t>;

} // namespace cairn::support
