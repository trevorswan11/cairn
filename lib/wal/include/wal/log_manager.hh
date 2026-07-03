#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <stdx/memory.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "error.hh"
#include "wal/record.hh"

namespace cairn::wal {

// A thread safe manager of log appending and flushing
class log_manager {
  public:
    explicit log_manager(std::filesystem::path log_path,
                         usize                 buffer_size = stdx::sizes::kib(64UZ)) noexcept
        : log_path_{std::move(log_path)}, buffer_size_{buffer_size} {};
    ~log_manager();
    MAKE_PINNED(log_manager)

    // Appends a record to the active bugger, assigning its lsn and triggering swap if needed
    [[nodiscard]] auto append_record(log_record& record) -> result<lsn_t>;

    // Wait until the record with the given lsn has been flushed to disk
    [[nodiscard]] auto flush(lsn_t lsn) -> result<void>;
    [[nodiscard]] auto flushed_lsn() const noexcept -> lsn_t {
        return flushed_lsn_.load(std::memory_order_relaxed);
    }

  private:
    auto flush_loop() -> void;
    auto trigger_buffer_swap() -> void;

  private:
    const std::filesystem::path log_path_;
    const usize                 buffer_size_;

    std::mutex              mutex_;
    std::condition_variable append_cv_;
    std::condition_variable flush_cv_;

    std::vector<std::byte> active_buffer_;
    std::vector<std::byte> flush_buffer_;

    std::atomic<lsn_t> next_lsn_{lsn_t{1}};
    std::atomic<lsn_t> flushed_lsn_{lsn_t{0}};

    std::thread       flush_thread_;
    std::atomic<bool> running_{true};
    std::atomic<bool> swap_requested_{false};
};

} // namespace cairn::wal
