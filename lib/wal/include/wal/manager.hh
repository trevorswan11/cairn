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
#include "wal/record.hh"

namespace cairn::wal {

// A thread safe manager of log appending and flushing
class manager {
  public:
    explicit manager(std::filesystem::path log_path,
                     usize                 buffer_size = stdx::sizes::kib(64UZ)) noexcept;
    ~manager();
    MAKE_PINNED(manager)

    // Appends a record to the active bugger, assigning its lsn and triggering swap if needed
    [[nodiscard]] auto append_record(record& record) -> result<lsn_t>;

    // Wait until the record with the given lsn has been flushed to disk
    [[nodiscard]] auto flush(lsn_t lsn) -> result<void>;
    [[nodiscard]] auto flushed_lsn() const noexcept -> lsn_t {
        return flushed_lsn_.load(std::memory_order_relaxed);
    }

  private:
    // The caller must hold the mutex
    auto trigger_buffer_swap() -> void;
    auto flush_loop() -> void;

    [[nodiscard]] auto next_lsn() noexcept -> lsn_t;
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

    std::atomic<lsn_t> next_lsn_{lsn_t{1}};
    std::atomic<lsn_t> flushed_lsn_{lsn_t{0}};

    std::jthread      flush_thread_;
    std::atomic<bool> running_{true};
    std::atomic<bool> swap_requested_{false};
    lsn_t             highest_lsn_to_flush_{INVALID_LSN};
};

} // namespace cairn::wal
