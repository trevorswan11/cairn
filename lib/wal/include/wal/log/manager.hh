#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include <stdx/memory.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/error.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"

namespace cairn::wal::log {

// A thread safe manager of log appending and flushing
class manager {
  public:
    explicit manager(std::filesystem::path log_path,
                     usize                 buffer_size = stdx::sizes::kib(64UZ)) noexcept;
    ~manager();
    MAKE_PINNED(manager)

    // Appends a record to the active bugger, assigning its lsn and triggering swap if needed
    [[nodiscard]] auto append_record(record& record) -> result<seq_num>;

    // Wait until the record with the given lsn has been flushed to disk
    [[nodiscard]] auto flush(seq_num lsn) -> result<void>;
    [[nodiscard]] auto flushed_lsn() const noexcept -> seq_num {
        return flushed_lsn_.load(std::memory_order_relaxed);
    }

    auto set_lsn_watermarks(seq_num last_lsn) noexcept -> void {
        std::scoped_lock lock{mutex_};
        next_lsn_.store(seq_num{std::to_underlying(last_lsn) + 1}, std::memory_order_relaxed);
        flushed_lsn_.store(last_lsn, std::memory_order_relaxed);
    }

  private:
    // The caller must hold the mutex
    auto trigger_buffer_swap() -> void;
    auto flush_loop() -> void;

    [[nodiscard]] auto next_lsn() noexcept -> seq_num;
    [[nodiscard]] auto running() const noexcept -> bool {
        return running_.load(std::memory_order_relaxed);
    }

  private:
    const std::filesystem::path log_path_;
    const usize                 buffer_size_;

    std::mutex              mutex_;
    std::condition_variable append_cv_;
    std::condition_variable flush_cv_;

    std::vector<std::byte> active_buffer_;
    std::vector<std::byte> flush_buffer_;

    std::atomic<seq_num> next_lsn_{seq_num{1}};
    std::atomic<seq_num> flushed_lsn_{seq_num{0}};

    std::jthread      flush_thread_;
    std::atomic<bool> running_{true};
    std::atomic<bool> swap_requested_{false};
    seq_num           highest_lsn_to_flush_{INVALID_LSN};
};

} // namespace cairn::wal::log
