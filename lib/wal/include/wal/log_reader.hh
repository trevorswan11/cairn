#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <utility>

#include <stdx/types.hh>
#include <vector>

#include "support/error.hh"
#include "support/io_utils.hh"
#include "wal/log_record.hh"

namespace cairn::wal {

// An bidirectional iterator over a log file of records
class log_reader {
  public:
    [[nodiscard]] static auto open(std::filesystem::path log_path) -> result<log_reader>;

    // The returned record's data is only valid until the next `next` or `prev` call
    [[nodiscard]] auto next() -> result<log_record>;

    // The returned record's data is only valid until the next `next` or `prev` call
    [[nodiscard]] auto prev() -> result<log_record>;

    auto seek_to_start() -> result<void> { return io_utils::seek_to_beg(file_); }
    auto seek_to_end() -> result<void> { return io_utils::seek_to_end(file_); }

  private:
    log_reader(std::ifstream file, i64 file_size) : file_{std::move(file)}, file_size_{file_size} {}

    [[nodiscard]] auto deserialize_record() noexcept -> result<log_record>;

  private:
    std::ifstream          file_;
    i64                    file_size_;
    std::vector<std::byte> record_buffer_;
};

} // namespace cairn::wal
