#include "storage/rw_latch.hh"

#include <stdx/profiler.hh>

#ifndef NDEBUG
#    include <stdx/assert.hh>
#endif

namespace cairn::storage {

auto rw_latch::lock() -> void {}

auto rw_latch::try_lock() -> bool { return false; }

auto rw_latch::unlock() -> void {}

auto rw_latch::lock_shared() -> void {}

auto rw_latch::try_lock_shared() -> bool { return false; }

auto rw_latch::unlock_shared() -> void {}

auto rw_latch::note_exclusive_acquired() noexcept -> void {}

auto rw_latch::note_exclusive_released() noexcept -> void {}

auto rw_latch::note_shared_acquired() noexcept -> void {}

auto rw_latch::note_shared_released() noexcept -> void {}

} // namespace cairn::storage
