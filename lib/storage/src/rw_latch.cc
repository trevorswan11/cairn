#include "storage/rw_latch.hh"

#include <stdx/profiler.hh>

#ifndef NDEBUG
#    include <stdx/assert.hh>
#endif

namespace cairn::storage {

auto rw_latch::lock() -> void {
    {
        PROFILE_SCOPE("rw_latch::lock");
        mutex_.lock();
    }
    note_exclusive_acquired();
}

auto rw_latch::try_lock() -> bool {
    if (!mutex_.try_lock()) { return false; }
    note_exclusive_acquired();
    return true;
}

auto rw_latch::unlock() -> void {
    mutex_.unlock();
    note_exclusive_released();
}

auto rw_latch::lock_shared() -> void {
    {
        PROFILE_SCOPE("rw_latch::try_lock");
        mutex_.lock_shared();
    }
    note_shared_acquired();
}

auto rw_latch::try_lock_shared() -> bool {
    if (!mutex_.try_lock_shared()) { return false; }
    note_shared_acquired();
    return true;
}

auto rw_latch::unlock_shared() -> void {
    mutex_.unlock_shared();
    note_shared_released();
}

auto rw_latch::note_exclusive_acquired() noexcept -> void {
#ifndef NDEBUG
    ASSERT(readers_.load() == 0, "exclusive lock acquired while readers present");
    ASSERT(!writer_.exchange(true), "exclusive lock acquired while already write-held");
#endif
}

auto rw_latch::note_exclusive_released() noexcept -> void {
#ifndef NDEBUG
    ASSERT(writer_.exchange(false), "exclusive unlock without a matching lock");
#endif
}

auto rw_latch::note_shared_acquired() noexcept -> void {
#ifndef NDEBUG
    ASSERT(!writer_.load(), "shared lock acquired while write-held");
    readers_.fetch_add(1);
#endif
}

auto rw_latch::note_shared_released() noexcept -> void {
#ifndef NDEBUG
    ASSERT(readers_.fetch_sub(1) > 0, "shared unlock without a matching lock");
#endif
}

} // namespace cairn::storage
