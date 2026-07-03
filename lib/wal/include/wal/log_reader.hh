#pragma once

#include <filesystem>
#include <fstream>

#include <stdx/types.hh>

#include "error.hh"
#include "wal/log_record.hh"

namespace cairn::wal {

class log_reader {
  public:
    explicit log_reader(std::filesystem::path path);

    [[nodiscard]] auto has_next() -> bool;
    [[nodiscard]] auto next() -> result<log_record>;

    [[nodiscard]] auto has_prev() -> bool;
    [[nodiscard]] auto prev() -> result<log_record>;

    auto seek_to_start() -> result<void>;
    auto seek_to_end() -> result<void>;

  private:
    std::ifstream        file_;
    [[maybe_unused]] u64 file_size_;
};

} // namespace cairn::wal
