#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <utility>

#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/error.hh"
#include "storage/page.hh"

namespace cairn::storage {

using read_buf_t  = gsl::span<std::byte, DB_PAGE_SIZE>;
using write_buf_t = gsl::span<const std::byte, DB_PAGE_SIZE>;

// Owns the single backing file and moves pages from it to provided buffers
//
// All queries and operations are thread safe
class disk_manager {
  public:
    ~disk_manager();
    MAKE_PINNED(disk_manager);

    // Opens or creates a file at the provided path
    [[nodiscard]] static auto open(const std::filesystem::path& path)
        -> result<stdx::box<disk_manager>>;

    // Grows the file by one fully zeroed page and returns its new id
    [[nodiscard]] auto allocate_page() -> result<page_id_t>;

    // The destination pointer must be at least PAGE_SIZE size
    [[nodiscard]] auto read_page(page_id_t pid, read_buf_t buf) -> result<void>;
    [[nodiscard]] auto read_page(page_id_t pid, std::byte* buf) -> result<void> {
        return read_page(pid, read_buf_t{buf, DB_PAGE_SIZE});
    }

    // Writes and flushes the buffer out to the file
    [[nodiscard]] auto write_page(page_id_t pid, write_buf_t buf) -> result<void>;
    [[nodiscard]] auto write_page(page_id_t pid, const std::byte* buf) -> result<void> {
        return write_page(pid, write_buf_t{buf, DB_PAGE_SIZE});
    }

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
    i64                num_pages_{0};
};

} // namespace cairn::storage
