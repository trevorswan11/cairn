#include "wal/log_manager.hh"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <stdx/assert.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/error.hh"
#include "wal/log_record.hh"

namespace cairn::wal {

log_manager::log_manager(std::filesystem::path log_path, usize buffer_size) noexcept
    : log_path_{std::move(log_path)}, buffer_size_{buffer_size} {
    active_buffer_.reserve(buffer_size_);
    flush_buffer_.reserve(buffer_size_);
    flush_thread_ = std::jthread{&log_manager::flush_loop, this};
};

log_manager::~log_manager() {
    PROFILE_FUNCTION();

    // Halt flush execution and force a final swap if needed
    {
        std::scoped_lock lock{mutex_};
        running_.store(false, std::memory_order_relaxed);
        append_cv_.notify_all();
        flush_cv_.notify_all();
    }

    if (flush_thread_.joinable()) { flush_thread_.join(); }
}

auto log_manager::append_record(log_record& record) -> result<lsn_t> {
    PROFILE_FUNCTION();

    // Staging is done locally to allow for lock-free operation
    thread_local std::vector<std::byte> staging;
    staging.clear();

    std::unique_lock lock{mutex_};

    // Write in the lsn before serializing
    const auto lsn{next_lsn()};
    record.lsn = lsn;
    record.serialize(staging);

    // Spin until there is space in the active buffer
    while (active_buffer_.size() + staging.size() > buffer_size_) {
        if (swap_requested_.load(std::memory_order_relaxed)) {
            append_cv_.wait(lock);
        } else {
            trigger_buffer_swap();
        }
    }

    active_buffer_.insert_range(active_buffer_.end(), staging);
    return lsn;
}

auto log_manager::flush(lsn_t lsn) -> result<void> {
    PROFILE_FUNCTION();

    std::unique_lock lock{mutex_};
    if (flushed_lsn_.load(std::memory_order_relaxed) >= lsn) { return {}; }

    // Triggers swaps if woken up and LSN is still not flushed
    while (flushed_lsn_.load(std::memory_order_relaxed) < lsn && running()) {
        if (!swap_requested_.load(std::memory_order_relaxed) && !active_buffer_.empty()) {
            trigger_buffer_swap();
        }
        flush_cv_.wait(lock);
    }

    if (!running() && flushed_lsn_.load(std::memory_order_relaxed) < lsn) {
        return stdx::err{error_t::IO_ERROR};
    }
    return {};
}

auto log_manager::trigger_buffer_swap() -> void {
    PROFILE_FUNCTION();
    std::swap(active_buffer_, flush_buffer_);
    highest_lsn_to_flush_ =
        lsn_t{std::to_underlying(next_lsn_.load(std::memory_order_relaxed)) - 1};
    swap_requested_.store(true, std::memory_order_relaxed);
    append_cv_.notify_all();
}

auto log_manager::flush_loop() -> void {
    std::ofstream out{log_path_, std::ios::out | std::ios::binary | std::ios::app};
    VERIFY(out.is_open(), "Background flush thread could not open file");

    std::unique_lock lock{mutex_};
    while (running() || swap_requested_.load(std::memory_order_relaxed) ||
           !active_buffer_.empty()) {
        PROFILE_SCOPE("log_manager flush loop iteration");
        append_cv_.wait(lock, [&] {
            return swap_requested_.load(std::memory_order_relaxed) || !running() ||
                   (!active_buffer_.empty() && !running());
        });

        // Trigger the swap on shutdown if we have active data but no current swap in progress
        if (!swap_requested_.load(std::memory_order_relaxed) && !running() &&
            !active_buffer_.empty()) {
            trigger_buffer_swap();
        }

        if (swap_requested_.load(std::memory_order_relaxed)) {
            // Release the lock for concurrent file I/O
            lock.unlock();

            out.write(reinterpret_cast<const char*>(flush_buffer_.data()),
                      static_cast<std::streamsize>(flush_buffer_.size()));
            out.flush(); // Sync to the OS
            lock.lock();

            // Mark the flush complete and notify the waiting threads
            flushed_lsn_.store(highest_lsn_to_flush_, std::memory_order_release);
            flush_buffer_.clear();
            swap_requested_.store(false, std::memory_order_relaxed);

            append_cv_.notify_all();
            flush_cv_.notify_all();
        }
    }
}

auto log_manager::next_lsn() noexcept -> lsn_t {
    PROFILE_FUNCTION();
    lsn_t current{next_lsn_.load(std::memory_order_relaxed)}, next{INVALID_LSN};
    do {
        next = lsn_t{std::to_underlying(current) + 1};
    } while (!next_lsn_.compare_exchange_weak(current, next, std::memory_order_relaxed));
    return current;
}

} // namespace cairn::wal
