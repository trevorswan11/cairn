#pragma once

#include <stdx/result.hh>
#include <stdx/types.hh>

namespace cairn::storage {

enum class error_t : u8 {
    IO_ERROR,        // a read/write/seek against the backing file failed
    INVALID_PAGE_ID, // an illegal page id was supplied
    SHORT_READ,      // fewer than PAGE_SIZE bytes were read from disk

    POOL_EXHAUSTED, // every frame is pinned and nothing is evictable
    PAGE_NOT_FOUND, // the requested page id is not resident in the pool

    TREE_CORRUPT, // An invariant was violated in the storage engine's tree
};

template <typename T> using result = stdx::result<T, error_t>;

} // namespace cairn::storage
