#include "wal/log/reader.hh"

#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>

#include <gsl/span>
#include <stdx/option.hh>
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

auto reader::has_next() noexcept -> result<stdx::option<i64>> {
    const i64 pos{file_.tellg()};
    if (pos < 0) { return stdx::err{error_t::IO_ERROR}; }
    if (pos == file_size_) { return stdx::none; }
    if (file_size_ - pos < record::MINIMUM_SIZE<i64>) {
        return stdx::err{error_t::WAL_SIZE_CORRUPT};
    }
    return pos;
}

auto reader::next(stdx::option<i64> pos) -> result<record> {
    PROFILE_FUNCTION();
    i64 true_pos;
    if (!pos) {
        const auto next_pos{TRY(has_next())};
        if (!next_pos) { return stdx::err{error_t::WAL_EOF}; }
        true_pos = *next_pos;
    } else {
        true_pos = *pos;
    }

    std::array<char, 4> size_buf;
    i32                 size{0};

    while (true) {
        file_.clear();
        errno = 0;
        file_.seekg(true_pos, std::ios::beg);
        if (file_.fail()) {
            if (io_utils::interrupted()) { continue; }
            return stdx::err{error_t::IO_ERROR};
        }

        errno = 0;
        file_.read(size_buf.data(), sizeof(size_buf));
        if (file_.fail()) {
            if (io_utils::interrupted()) { continue; }
            return stdx::err{error_t::IO_ERROR};
        }

        size = std::bit_cast<i32>(size_buf);
        if (size < record::MINIMUM_SIZE<i32> || (true_pos + size > file_size_)) {
            return stdx::err{error_t::WAL_SIZE_CORRUPT};
        }

        record_buffer_.clear();
        record_buffer_.resize(static_cast<usize>(size));
        std::memcpy(record_buffer_.data(), size_buf.data(), sizeof(size_buf));

        errno = 0;
        file_.read(reinterpret_cast<char*>(record_buffer_.data() + sizeof(size_buf)),
                   size - static_cast<std::streamoff>(sizeof(size_buf)));
        if (!file_.fail()) { break; }

        if (io_utils::interrupted()) { continue; }
        return stdx::err{error_t::IO_ERROR};
    }

    return deserialize_record();
}

auto reader::has_prev() noexcept -> result<stdx::option<i64>> {
    const i64 pos{file_.tellg()};
    if (pos < 0) { return stdx::err{error_t::IO_ERROR}; }
    if (pos == 0) { return stdx::none; }
    if (pos < record::MINIMUM_SIZE<i64>) { return stdx::err{error_t::WAL_SIZE_CORRUPT}; }
    return pos;
}

auto reader::prev(stdx::option<i64> pos) -> result<record> {
    PROFILE_FUNCTION();
    i64 true_pos;
    if (!pos) {
        const auto prev_pos{TRY(has_prev())};
        if (!prev_pos) { return stdx::err{error_t::WAL_EOF}; }
        true_pos = *prev_pos;
    } else {
        true_pos = *pos;
    }

    std::array<char, 4> size_buf;
    i32                 size{0};
    i64                 start_pos{0};

    while (true) {
        file_.clear();
        errno = 0;
        file_.seekg(true_pos - static_cast<i64>(sizeof(size_buf)), std::ios::beg);
        if (file_.fail()) {
            if (io_utils::interrupted()) { continue; }
            return stdx::err{error_t::IO_ERROR};
        }

        errno = 0;
        file_.read(size_buf.data(), sizeof(size_buf));
        if (file_.fail()) {
            if (io_utils::interrupted()) { continue; }
            return stdx::err{error_t::IO_ERROR};
        }

        size = std::bit_cast<i32>(size_buf);
        if (size < record::MINIMUM_SIZE<i32> || true_pos < size) {
            return stdx::err{error_t::WAL_SIZE_CORRUPT};
        }

        record_buffer_.clear();
        record_buffer_.resize(static_cast<usize>(size));
        std::memcpy(
            record_buffer_.data() + size - sizeof(size_buf), size_buf.data(), sizeof(size_buf));

        start_pos = true_pos - size;
        errno     = 0;
        file_.seekg(start_pos, std::ios::beg);
        if (file_.fail()) {
            if (io_utils::interrupted()) { continue; }
            return stdx::err{error_t::IO_ERROR};
        }

        errno = 0;
        file_.read(reinterpret_cast<char*>(record_buffer_.data()),
                   size - static_cast<std::streamoff>(sizeof(size_buf)));
        if (file_.fail()) {
            if (io_utils::interrupted()) { continue; }
            return stdx::err{error_t::IO_ERROR};
        }

        // Reset the file to the start of the record for the next call
        errno = 0;
        file_.seekg(start_pos, std::ios::beg);
        if (!file_.fail()) { break; }

        if (io_utils::interrupted()) { continue; }
        return stdx::err{error_t::IO_ERROR};
    }

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
