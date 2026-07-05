#pragma once

#include <stdx/types.hh>

namespace cairn::crash {

// Indicates that boundaries should be respected
//
// Resets all boundary limits and counters
auto initialize() noexcept -> void;

// Configured such that after `limit` boundaries, the program crashes
auto configure(i32 limit) noexcept -> void;

enum class boundary_t : u8 {
    DISK_WRITE_BEFORE, // Page/zeros not yet written to file stream
    DISK_WRITE_AFTER,  // Page written to stream, but not yet flushed to OS
    DISK_FLUSH_AFTER,  // Page flushed to OS

    WAL_WRITE_BEFORE, // WAL buffer not yet written to log file stream
    WAL_WRITE_AFTER,  // WAL buffer written to stream, but not yet flushed
    WAL_FLUSH_AFTER,  // WAL buffer flushed to OS
};

// No-op if not initialized, otherwise this may crash the program
auto test_boundary(boundary_t boundary) noexcept -> void;

} // namespace cairn::crash
