#include "wal/log/reader.hh"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>

#include <gsl/span>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/error.hh"
#include "support/io_utils.hh"
#include "wal/log/record.hh"

namespace cairn::wal::log {

auto reader::open(std::filesystem::path log_path) -> result<reader> {
    PROFILE_FUNCTION();
    std::ifstream file{log_path, std::ios::in | std::ios::binary};
    if (!file.is_open()) { return stdx::err{error_t::WAL_LOG_FILE_NOT_FOUND}; }

    TRY(io_utils::seek_to_end(file));
    const auto end{file.tellg()};
    if (end < 0) { return stdx::err{error_t::IO_ERROR}; }
    TRY(io_utils::seek_to_beg(file));

    return reader{std::move(file), end};
}

auto reader::next() -> result<record> {
    PROFILE_FUNCTION();
    const i64 pos{file_.tellg()};
    if (pos < 0) { return stdx::err{error_t::IO_ERROR}; }
    if (pos == file_size_) { return stdx::err{error_t::WAL_EOF}; }
    if (file_size_ - pos < record::MINIMUM_SIZE<i64>) {
        return stdx::err{error_t::WAL_SIZE_CORRUPT};
    }

    std::array<char, 4> size_buf;
    file_.read(size_buf.data(), sizeof(size_buf));
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

    const auto size{std::bit_cast<i32>(size_buf)};
    if (size < record::MINIMUM_SIZE<i32> || (pos + size > file_size_)) {
        return stdx::err{error_t::WAL_SIZE_CORRUPT};
    }

    record_buffer_.clear();
    record_buffer_.resize(static_cast<usize>(size));
    std::memcpy(record_buffer_.data(), size_buf.data(), sizeof(size_buf));

    file_.read(reinterpret_cast<char*>(record_buffer_.data() + sizeof(size_buf)),
               size - static_cast<std::streamoff>(sizeof(size_buf)));
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

    return deserialize_record();
}

auto reader::prev() -> result<record> {
    PROFILE_FUNCTION();
    const i64 pos{file_.tellg()};
    if (pos < 0) { return stdx::err{error_t::IO_ERROR}; }
    if (pos == 0) { return stdx::err{error_t::WAL_EOF}; }
    if (pos < record::MINIMUM_SIZE<i64>) { return stdx::err{error_t::WAL_SIZE_CORRUPT}; }

    std::array<char, 4> size_buf;
    file_.seekg(pos - static_cast<i64>(sizeof(size_buf)));
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }
    file_.read(size_buf.data(), sizeof(size_buf));
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

    const auto size{std::bit_cast<i32>(size_buf)};
    if (size < record::MINIMUM_SIZE<i32> || pos < size) {
        return stdx::err{error_t::WAL_SIZE_CORRUPT};
    }

    record_buffer_.clear();
    record_buffer_.resize(static_cast<usize>(size));
    std::memcpy(record_buffer_.data() + size - sizeof(size_buf), size_buf.data(), sizeof(size_buf));

    const auto start_pos{pos - size};
    file_.seekg(start_pos);
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }
    file_.read(reinterpret_cast<char*>(record_buffer_.data()),
               size - static_cast<std::streamoff>(sizeof(size_buf)));
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

    // Reset the file to the start of the record for the next call
    file_.seekg(start_pos);
    if (file_.fail()) { return stdx::err{error_t::IO_ERROR}; }

    return deserialize_record();
}

auto reader::deserialize_record() noexcept -> result<record> {
    PROFILE_FUNCTION();
    gsl::span<const std::byte> buf{record_buffer_};
    const auto                 record{TRY(record::deserialize(buf))};
    if (!buf.empty()) { return stdx::err{error_t::WAL_SIZE_CORRUPT}; }
    return std::move(record);
}

} // namespace cairn::wal::log
