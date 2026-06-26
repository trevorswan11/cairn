#pragma once

#include <stdx/result.hh>
#include <stdx/types.hh>

namespace cairn::storage {

enum class error_t : u8 {
    IO_ERROR,        // a read/write/seek against the backing file failed
    INVALID_PAGE_ID, // an illegal page id was supplied
    SHORT_READ,      // fewer than PAGE_SIZE bytes were read from disk
};

template <typename T> using result = stdx::result<T, error_t>;

} // namespace cairn::storage
