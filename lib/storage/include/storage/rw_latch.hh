#pragma once

#include <shared_mutex>

#include <stdx/utility.hh>

#ifndef NDEBUG
#    include <atomic>

#    include <stdx/types.hh>
#endif

namespace cairn::storage {

// A thin wrapper over a shared mutex for instrumented latching
class rw_latch {
  public:
    rw_latch() noexcept = default;
    ~rw_latch()         = default;
    MAKE_PINNED(rw_latch);

    auto               lock() -> void;
    [[nodiscard]] auto try_lock() -> bool;
    auto               unlock() -> void;

    auto               lock_shared() -> void;
    [[nodiscard]] auto try_lock_shared() -> bool;
    auto               unlock_shared() -> void;

  private:
    auto note_exclusive_acquired() noexcept -> void;
    auto note_exclusive_released() noexcept -> void;
    auto note_shared_acquired() noexcept -> void;
    auto note_shared_released() noexcept -> void;

  private:
    std::shared_mutex mutex_;
#ifndef NDEBUG
    std::atomic<i32>  readers_{0};
    std::atomic<bool> writer_{false};
#endif
};

} // namespace cairn::storage
