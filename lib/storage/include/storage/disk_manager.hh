#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

#include <stdx/memory.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/error.hh"
#include "storage/page.hh"

namespace cairn::storage {

// Owns the single backing file and moves pages from it to provided buffers
//
// All queries and operations are thread safe
class disk_manager {
  public:
    ~disk_manager();
    MAKE_PINNED(disk_manager)

    // Opens or creates a file at the provided path
    [[nodiscard]] static auto open(const std::filesystem::path& path)
        -> result<stdx::box<disk_manager>>;

    // Grows the file by one fully zeroed page and returns its new id
    [[nodiscard]] auto allocate_page() -> result<page_id_t>;

    // The destination pointer must be at least PAGE_SIZE size
    [[nodiscard]] auto read_page(page_id_t pid, std::byte* buf) -> result<void>;

    // Writes and flushes the buffer out to the file
    [[nodiscard]] auto write_page(page_id_t pid, const std::byte* buf) -> result<void>;

    // The number of pages the underlying file currently spans
    [[nodiscard]] auto num_pages() const noexcept -> i64 {
        const std::scoped_lock lock{latch_};
        return num_pages_;
    }

  private:
    disk_manager(std::fstream file, i64 num_pages)
        : file_{std::move(file)}, num_pages_{num_pages} {}

  private:
    mutable std::mutex latch_;
    std::fstream       file_;
    i64                num_pages_;
};

} // namespace cairn::storage
