#pragma once

#include <stdx/result.hh>
#include <stdx/types.hh>

namespace cairn {

// The central error type for all of cairn
enum class error_t : u8 {
    IO_ERROR,          // a read/write/seek against the backing file failed
    SUBPROCESS_FAILED, // could not launch subprocess

    SUPPORT_JSON_PARSE_ERROR,
    SUPPORT_JSON_TYPE_ERROR,
    SUPPORT_JSON_MISSING_FIELD,

    STORAGE_INVALID_PAGE_ID, // an illegal page id was supplied
    STORAGE_SHORT_READ,      // fewer than PAGE_SIZE bytes were read from disk

    STORAGE_POOL_EXHAUSTED, // every frame is pinned and nothing is evictable
    STORAGE_PAGE_NOT_FOUND, // the requested page id is not resident in the pool

    STORAGE_TREE_CORRUPT,  // an invariant was violated in the storage engine's tree
    STORAGE_KEY_NOT_FOUND, // erase/lookup against a key that is not present
    STORAGE_DUPLICATE_KEY, // insert hit an existing key

    STORAGE_PAGE_FULL,     // insufficient free space to insert/update a tuple
    STORAGE_INVALID_SLOT,  // out of bounds slot id
    STORAGE_TUPLE_DELETED, // slot points to a tombstoned tuple

    SQL_VALUE_SCHEMA_COUNT_MISMATCH, // the value count did not match the schema count
    SQL_INVALID_COLUMN_INDEX,        // attempt to get value not in schema column range
    SQL_UNSUPPORTED_FIXED_TYPE,      // attempt to get a type unknown to the tuple deserializer

    WAL_SOURCE_BUF_TOO_SMALL, // the reader buf did not have enough space
    WAL_SIZE_CORRUPT,       // deserializing a log record resulted in a header/footer size mismatch
    WAL_CHECKSUM_CORRUPT,   // deserializing a log record resulted in a different checksum
    WAL_LOG_FILE_NOT_FOUND, // the provided filepath did not resolve to a file on disk
    WAL_EOF, // a prev or next read could not succeed as the file is at the start or end
    WAL_CONTROL_PATH_NOT_FOUND, // the provided checkpoint control path does not exist

    TXN_NOT_FOUND, // the requested transaction id is not active
};

template <typename T> using result = stdx::result<T, error_t>;

} // namespace cairn
