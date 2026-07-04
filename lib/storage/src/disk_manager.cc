#include "storage/disk_manager.hh"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <utility>

#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/page.hh"
#include "support/error.hh"
#include "support/io_utils.hh"

namespace cairn::storage {

namespace {

[[nodiscard]] auto page_offset(page_id_t pid) noexcept -> std::streamoff {
    return static_cast<std::streamoff>(std::to_underlying(pid)) *
           static_cast<std::streamoff>(DB_PAGE_SIZE);
}

} // namespace

disk_manager::~disk_manager() {
    // Don't rely on fstream RAII since the lock must gate the close
    std::scoped_lock lock{mutex_};
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

auto disk_manager::open(const std::filesystem::path& path) -> result<stdx::box<disk_manager>> {
    PROFILE_FUNCTION();
    constexpr auto open_mode{std::ios::in | std::ios::out | std::ios::binary};

    std::fstream file;
    file.open(path, open_mode);
    if (!file.is_open()) {
        // A file not opening might mean creation is necessary
        {
            std::fstream create{path, std::ios::out | std::ios::binary};
            if (!create.is_open()) { return stdx::err{error_t::IO_ERROR}; }
        }
        file.open(path, open_mode);
        if (!file.is_open()) { return stdx::err{error_t::IO_ERROR}; }
    }

    TRY(io_utils::seek_to_end(file));
    const auto end{file.tellg()};
    if (end < 0) { return stdx::err{error_t::IO_ERROR}; }

    // Ceiling division is needed to that partial pages are not dropped on open
    const auto     end_pos{static_cast<i64>(end)};
    constexpr auto page_sz{static_cast<i64>(DB_PAGE_SIZE)};
    const auto     num_pages{(end_pos + page_sz - 1) / page_sz};

    return stdx::box<disk_manager>{new disk_manager{std::move(file), num_pages}};
}

auto disk_manager::allocate_page() -> result<page_id_t> {
    PROFILE_FUNCTION();
    std::scoped_lock lock{mutex_};
    const page_id_t  pid{num_pages_};

    static constexpr std::array<std::byte, DB_PAGE_SIZE> zeros{};
    file_.clear();
    file_.seekp(page_offset(pid), std::ios::beg);
    file_.write(reinterpret_cast<const char*>(zeros.data()), zeros.size());
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }
    file_.flush();
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

    num_pages_ += 1;
    return pid;
}

auto disk_manager::read_page(page_id_t pid, read_buf_t buf) -> result<void> {
    PROFILE_FUNCTION();
    std::scoped_lock lock{mutex_};
    const auto       id{std::to_underlying(pid)};
    if (id < 0 || id >= num_pages_) { return stdx::err{error_t::STORAGE_INVALID_PAGE_ID}; }

    file_.clear();
    file_.seekg(page_offset(pid), std::ios::beg);
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

    usize bytes_read{0};
    while (bytes_read < DB_PAGE_SIZE) {
        file_.read(reinterpret_cast<char*>(buf.data()) + bytes_read,
                   static_cast<std::streamsize>(DB_PAGE_SIZE - bytes_read));

        const auto count{static_cast<usize>(file_.gcount())};
        bytes_read += count;
        if (bytes_read == DB_PAGE_SIZE) { break; }

        if (file_.eof()) { return stdx::err{error_t::STORAGE_SHORT_READ}; }
        if (file_.fail()) {
            if (io_utils::interrupted()) {
                file_.clear();
                continue;
            }
            return stdx::err{error_t::STORAGE_SHORT_READ};
        }
    }
    return {};
}

auto disk_manager::write_page(page_id_t pid, write_buf_t buf) -> result<void> {
    PROFILE_FUNCTION();
    std::scoped_lock lock{mutex_};
    const auto       id{std::to_underlying(pid)};
    if (id < 0 || id >= num_pages_) { return stdx::err{error_t::STORAGE_INVALID_PAGE_ID}; }

    usize bytes_written{0};
    while (bytes_written < DB_PAGE_SIZE) {
        file_.clear();
        file_.seekp(page_offset(pid), std::ios::beg);
        if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

        file_.write(reinterpret_cast<const char*>(buf.data()), DB_PAGE_SIZE);
        if (!file_.fail()) {
            bytes_written = DB_PAGE_SIZE;
            break;
        }
        if (io_utils::interrupted()) { continue; }
        return stdx::err{error_t::IO_ERROR};
    }

    file_.clear();
    file_.flush();
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }
    return {};
}

} // namespace cairn::storage
