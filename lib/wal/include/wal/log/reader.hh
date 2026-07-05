#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/error.hh"
#include "support/io_utils.hh"
#include "wal/log/record.hh"

namespace cairn::wal::log {

// An bidirectional iterator over a log file of records
class reader {
  public:
    [[nodiscard]] static auto open(std::filesystem::path log_path) -> result<reader>;

    // Returns the position of the next entry if present
    [[nodiscard]] auto has_next() noexcept -> result<stdx::option<i64>>;

    // The returned record's data is only valid until the next `next` or `prev` call
    //
    // Passing a position bypasses positional lookup of the next logical record
    [[nodiscard]] auto next(stdx::option<i64> pos = stdx::none) -> result<record>;

    // Returns the position of the previous entry if present
    [[nodiscard]] auto has_prev() noexcept -> result<stdx::option<i64>>;

    // The returned record's data is only valid until the next `next` or `prev` call
    //
    // Passing a position bypasses positional lookup of the next logical record
    [[nodiscard]] auto prev(stdx::option<i64> pos = stdx::none) -> result<record>;

    auto seek_to_start() -> result<void> { return io_utils::seek_to_beg(file_); }
    auto seek_to_end() -> result<void> { return io_utils::seek_to_end(file_); }

  private:
    reader(std::ifstream file, i64 file_size) : file_{std::move(file)}, file_size_{file_size} {}

    [[nodiscard]] auto deserialize_record() noexcept -> result<record>;

  private:
    std::ifstream          file_;
    i64                    file_size_;
    std::vector<std::byte> record_buffer_;
};

} // namespace cairn::wal::log
