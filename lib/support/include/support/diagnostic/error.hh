#pragma once

#include <ostream>
#include <string>
#include <string_view>

#include <fmt/base.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/diagnostic/location.hh"

namespace cairn {

// The central error type for all of cairn
enum class error : u8 {
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

    TXN_NOT_FOUND,               // the requested transaction id is not active
    TXN_DEADLOCK_DETECTED,       // transaction was aborted to prevent or resolve a deadlock
    TXN_LOCK_ACQUISITION_FAILED, // lock manager limits exceeded
    TXN_SERIALIZATION_FAILURE,   // validation failure

    SQL_EOF, // The sql lexer/parser reached the end of the token stream
};

template <typename T> using result = stdx::result<T, error>;

class diagnostic {
  public:
    enum class level : u8 {
        ERROR,
        WARNING,
    };

  public:
    constexpr explicit diagnostic(error err) noexcept : err_{err} {}
    constexpr diagnostic(error err, usize line, usize column) noexcept
        : loc_{{line, column}}, err_{err} {}
    template <Locateable T>
    constexpr diagnostic(error err, T t) : loc_{source_info<T>::get(t)}, err_{err} {}

    constexpr diagnostic(stdx::option<std::string> msg,
                         error                     err,
                         usize                     line,
                         usize                     column) noexcept
        : message_{std::move(msg)}, loc_{{line, column}}, err_{err} {}
    constexpr diagnostic(stdx::option<std::string> msg, error err) noexcept
        : message_{std::move(msg)}, err_{err} {}

    template <Locateable T>
    diagnostic(stdx::option<std::string> msg, error err, const T& t)
        : message_{std::move(msg)}, loc_{source_info<T>::get(t)}, err_{err} {}

    // Moves the passed diagnostic into a new one with an error code
    diagnostic(diagnostic&& other, error err) noexcept
        : message_{std::move(other.message_)}, loc_{other.loc_}, err_{err}, level_{other.level_} {}

    // Moves the passed diagnostic into a new one with a specified source location
    template <Locateable T>
    diagnostic(diagnostic&& other, const T& t)
        : message_{std::move(other.message_)}, loc_{source_info<T>::get(t)}, err_{other.err_},
          level_{other.level_} {}

    MAKE_GETTER(message, const stdx::option<std::string>&)
    MAKE_GETTER(loc, stdx::option<location>)
    MAKE_GETTER(err, error)
    MAKE_GETTER(level, stdx::option<level>)

    [[nodiscard]] constexpr auto operator==(const diagnostic& other) const noexcept
        -> bool = default;

    // Diagnostics always default to `ERROR`
    auto set_level(stdx::option<level> level) noexcept -> void { level_ = level; }
    auto format(std::ostream&                         os,
                const stdx::option<std::string_view>& source_path = stdx::none,
                stdx::option<bool> in_terminal = stdx::none) const -> std::ostream&;

  private:
    stdx::option<std::string> message_;
    stdx::option<location>    loc_;
    error                     err_;
    stdx::option<level>       level_{level::ERROR};
};

} // namespace cairn
