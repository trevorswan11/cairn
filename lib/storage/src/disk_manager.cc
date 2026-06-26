#include "storage/disk_manager.hh"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <utility>

#include <stdx/memory.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/error.hh"
#include "storage/page.hh"

namespace cairn::storage {

namespace {

[[nodiscard]] auto page_offset(page_id_t pid) noexcept -> std::streamoff {
    return static_cast<std::streamoff>(std::to_underlying(pid)) *
           static_cast<std::streamoff>(DB_PAGE_SIZE);
}

} // namespace

disk_manager::~disk_manager() {
    // Don't rely on fstream RAII since the lock must gate the close
    std::scoped_lock lock{latch_};
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

auto disk_manager::open(const std::filesystem::path& path) -> result<stdx::box<disk_manager>> {
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

    file.seekg(0, std::ios::end);
    if (file.fail()) { return stdx::err{error_t::IO_ERROR}; }
    const auto end{file.tellg()};
    if (end < 0) { return stdx::err{error_t::IO_ERROR}; }
    const auto num_pages{static_cast<i64>(end) / static_cast<i64>(DB_PAGE_SIZE)};

    return stdx::box<disk_manager>{new disk_manager{std::move(file), num_pages}};
}

auto disk_manager::allocate_page() -> result<page_id_t> {
    std::scoped_lock lock{latch_};
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

auto disk_manager::read_page(page_id_t pid, std::byte* buf) -> result<void> {
    std::scoped_lock lock{latch_};
    const auto       id{std::to_underlying(pid)};
    if (id < 0 || id >= num_pages_) { return stdx::err{error_t::INVALID_PAGE_ID}; }

    file_.clear();
    file_.seekg(page_offset(pid), std::ios::beg);
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

    file_.read(reinterpret_cast<char*>(buf), DB_PAGE_SIZE);
    if (static_cast<usize>(file_.gcount()) != DB_PAGE_SIZE) {
        return stdx::err{error_t::SHORT_READ};
    }
    return {};
}

auto disk_manager::write_page(page_id_t pid, const std::byte* buf) -> result<void> {
    std::scoped_lock lock{latch_};
    const auto       id{std::to_underlying(pid)};
    if (id < 0 || id >= num_pages_) { return stdx::err{error_t::INVALID_PAGE_ID}; }

    file_.clear();
    file_.seekg(page_offset(pid), std::ios::beg);
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

    file_.write(reinterpret_cast<const char*>(buf), DB_PAGE_SIZE);
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }
    file_.flush();
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }
    return {};
}

} // namespace cairn::storage
