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

auto reader::next_at(i64 offset) -> result<record> {
    PROFILE_FUNCTION();

    std::array<char, 4> size_buf;
    i32                 size{0};

    TRY(io_utils::run_io(file_, [&] -> result<void> {
        file_.seekg(offset, std::ios::beg);
        if (file_.fail()) { return {}; }

        file_.read(size_buf.data(), sizeof(size_buf));
        if (file_.fail()) { return {}; }

        size = std::bit_cast<i32>(size_buf);
        if (size < record::MINIMUM_SIZE<i32> || (offset + size > file_size_)) {
            return stdx::err{error_t::WAL_SIZE_CORRUPT};
        }

        record_buffer_.clear();
        record_buffer_.resize(static_cast<usize>(size));
        std::memcpy(record_buffer_.data(), size_buf.data(), sizeof(size_buf));

        file_.read(reinterpret_cast<char*>(record_buffer_.data() + sizeof(size_buf)),
                   size - static_cast<std::streamoff>(sizeof(size_buf)));
        return {};
    }));

    return deserialize_record();
}

auto reader::next_record() -> result<stdx::option<record>> {
    if (const auto next_pos{TRY(has_next())}) { return TRY(next_at(*next_pos)); }
    return stdx::none;
}

auto reader::next_record_lenient() -> result<stdx::option<record>> {
    auto rec_res{next_record()};
    if (!rec_res) {
        if (rec_res.error() == error_t::WAL_SIZE_CORRUPT ||
            rec_res.error() == error_t::WAL_CHECKSUM_CORRUPT) {
            return stdx::none;
        }
        return stdx::err{rec_res.error()};
    }
    return rec_res.value();
}

auto reader::has_prev() noexcept -> result<stdx::option<i64>> {
    const i64 pos{file_.tellg()};
    if (pos < 0) { return stdx::err{error_t::IO_ERROR}; }
    if (pos == 0) { return stdx::none; }
    if (pos < record::MINIMUM_SIZE<i64>) { return stdx::err{error_t::WAL_SIZE_CORRUPT}; }
    return pos;
}

auto reader::prev_at(i64 offset) -> result<record> {
    PROFILE_FUNCTION();

    std::array<char, 4> size_buf;
    i32                 size{0};
    i64                 start_pos{0};

    TRY(io_utils::run_io(file_, [&] -> result<void> {
        file_.seekg(offset - static_cast<i64>(sizeof(size_buf)), std::ios::beg);
        if (file_.fail()) { return {}; }

        file_.read(size_buf.data(), sizeof(size_buf));
        if (file_.fail()) { return {}; }

        size = std::bit_cast<i32>(size_buf);
        if (size < record::MINIMUM_SIZE<i32> || offset < size) {
            return stdx::err{error_t::WAL_SIZE_CORRUPT};
        }

        record_buffer_.clear();
        record_buffer_.resize(static_cast<usize>(size));
        std::memcpy(
            record_buffer_.data() + size - sizeof(size_buf), size_buf.data(), sizeof(size_buf));

        start_pos = offset - size;
        file_.seekg(start_pos, std::ios::beg);
        if (file_.fail()) { return {}; }

        file_.read(reinterpret_cast<char*>(record_buffer_.data()),
                   size - static_cast<std::streamoff>(sizeof(size_buf)));
        if (file_.fail()) { return {}; }

        // Reset the file to the start of the record for the next call
        file_.seekg(start_pos, std::ios::beg);
        return {};
    }));

    return deserialize_record();
}

auto reader::prev_record() -> result<stdx::option<record>> {
    if (const auto prev_pos{TRY(has_prev())}) { return TRY(prev_at(*prev_pos)); }
    return stdx::none;
}

auto reader::deserialize_record() noexcept -> result<record> {
    PROFILE_FUNCTION();
    gsl::span<const std::byte> buf{record_buffer_};
    const auto                 record{TRY(record::deserialize(buf))};
    if (!buf.empty()) { return stdx::err{error_t::WAL_SIZE_CORRUPT}; }
    return std::move(record);
}

} // namespace cairn::wal::log
