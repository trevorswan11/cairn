#include "wal/record.hh"

#include <cstddef>
#include <cstring>
#include <vector>

#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "wal/checkpoints.hh"
#include "wal/sequence_number.hh"

namespace cairn::wal {

namespace {

template <typename T>
auto write_arbitrary_at(std::vector<std::byte>& dest, const T& val, usize offset) -> void {
    std::memcpy(dest.data() + offset, &val, sizeof(T));
}

template <typename T> auto write_arbitrary(std::vector<std::byte>& dest, const T& val) -> void {
    const auto* bytes{reinterpret_cast<const std::byte*>(&val)};
    dest.insert(dest.cend(), bytes, bytes + sizeof(T));
}

// Consumes the type and advances the span
template <typename T> auto read_arbitrary(gsl::span<const std::byte>& src) -> T {
    T out;
    std::memcpy(&out, src.data(), sizeof(T));
    src = src.subspan(sizeof(T));
    return out;
}

} // namespace

auto record::serialize(std::vector<std::byte>& dest) const -> void {
    PROFILE_FUNCTION();
    const auto start{dest.size()};
    write_arbitrary(dest, size); // Will be overwritten
    write_arbitrary(dest, lsn);
    write_arbitrary(dest, prev_lsn);
    write_arbitrary(dest, txn_id);
    write_arbitrary(dest, type);

    // Type specific fields
    if (type == record_type::UPDATE) {
        write_arbitrary(dest, page_id);
        write_arbitrary(dest, slot_id);
        write_arbitrary(dest, static_cast<u32>(redo_data.size()));
        write_arbitrary(dest, static_cast<u32>(undo_data.size()));

        dest.insert_range(dest.cend(), redo_data);
        dest.insert_range(dest.cend(), undo_data);
    } else if (type == record_type::CLEAR) {
        write_arbitrary(dest, page_id);
        write_arbitrary(dest, slot_id);
        write_arbitrary(dest, static_cast<u32>(redo_data.size()));
        write_arbitrary(dest, undo_next_lsn);

        dest.insert_range(dest.cend(), redo_data);
    } else if (type == record_type::CHECKPOINT_END) {
        while ((dest.size() - start) % 8 != 0) { dest.emplace_back(std::byte{0}); }
        write_arbitrary(dest, static_cast<u32>(dpt.size()));
        while ((dest.size() - start) % 8 != 0) { dest.emplace_back(std::byte{0}); }
        if (!dpt.empty()) {
            const auto* bytes{reinterpret_cast<const std::byte*>(dpt.data())};
            dest.insert(dest.end(), bytes, bytes + dpt.size_bytes());
        }

        while ((dest.size() - start) % 8 != 0) { dest.emplace_back(std::byte{0}); }
        write_arbitrary(dest, static_cast<u32>(att.size()));
        while ((dest.size() - start) % 8 != 0) { dest.emplace_back(std::byte{0}); }
        if (!att.empty()) {
            const auto* bytes{reinterpret_cast<const std::byte*>(att.data())};
            dest.insert(dest.end(), bytes, bytes + att.size_bytes());
        }
    }

    const auto checksum_pos{dest.size()};
    write_arbitrary(dest, checksum); // Will be overwritten
    const auto size_copy_pos{dest.size()};
    write_arbitrary(dest, size); // Will be overwritten

    // Write out the sizes back into their placeholder positions
    const auto end{dest.size()};
    auto       written_size = static_cast<u32>(end - start);
    write_arbitrary_at(dest, written_size, start);
    write_arbitrary_at(dest, written_size, size_copy_pos);

    // The checksum doesn't include anything past itself
    gsl::span<const std::byte> checksum_bytes{dest.data() + start,
                                              static_cast<usize>(checksum_pos - start)};
    write_arbitrary_at(dest, stdx::crc::crc32(checksum_bytes), checksum_pos);
}

auto record::deserialize(gsl::span<const std::byte>& src) noexcept -> result<record> {
    PROFILE_FUNCTION();
    const auto original_span{src};
    if (src.size() < MINIMUM_SIZE<>) { return stdx::err{error_t::WAL_SOURCE_BUF_TOO_SMALL}; }

    const auto record_size{read_arbitrary<i32>(src)};
    if (original_span.size() < static_cast<usize>(record_size)) {
        return stdx::err{error_t::WAL_SOURCE_BUF_TOO_SMALL};
    }

    // Ensures we don't read outside of this record's data
    auto record_span{original_span.subspan(sizeof(record_size),
                                           static_cast<usize>(record_size) - sizeof(record_size))};
    src = original_span.subspan(static_cast<usize>(record_size));

    record record;
    record.size     = record_size;
    record.lsn      = read_arbitrary<lsn_t>(record_span);
    record.prev_lsn = read_arbitrary<stdx::option<lsn_t>>(record_span);
    record.txn_id   = read_arbitrary<txn::id_t>(record_span);
    record.type     = read_arbitrary<record_type>(record_span);

    // Type specific fields
    if (record.type == record_type::UPDATE) {
        record.page_id = read_arbitrary<stdx::option<storage::page_id_t>>(record_span);
        record.slot_id = read_arbitrary<stdx::option<storage::slot_id_t>>(record_span);
        const auto redo_len{read_arbitrary<u32>(record_span)};
        const auto undo_len{read_arbitrary<u32>(record_span)};

        record.redo_data = record_span.subspan(0, redo_len);
        record_span      = record_span.subspan(redo_len);
        record.undo_data = record_span.subspan(0, undo_len);
        record_span      = record_span.subspan(undo_len);
    } else if (record.type == record_type::CLEAR) {
        record.page_id = read_arbitrary<stdx::option<storage::page_id_t>>(record_span);
        record.slot_id = read_arbitrary<stdx::option<storage::slot_id_t>>(record_span);
        const auto redo_len{read_arbitrary<u32>(record_span)};
        record.undo_next_lsn = read_arbitrary<stdx::option<lsn_t>>(record_span);

        record.redo_data = record_span.subspan(0, redo_len);
        record_span      = record_span.subspan(redo_len);
    } else if (record.type == record_type::CHECKPOINT_END) {
        // This is needed due to some zero padding introduced in serialization
        auto align_span = [&](gsl::span<const std::byte>& span) {
            const auto offset{static_cast<usize>(span.data() - original_span.data())};
            const auto rem{offset % 8};
            if (rem != 0) { span = span.subspan(8 - rem); }
        };

        align_span(record_span);
        const auto dpt_len{read_arbitrary<u32>(record_span)};

        align_span(record_span);
        const auto dpt_bytes_len{dpt_len * sizeof(checkpoint_dpt_entry)};
        record.dpt = gsl::span<const checkpoint_dpt_entry>{
            reinterpret_cast<const checkpoint_dpt_entry*>(record_span.data()), dpt_len};
        record_span = record_span.subspan(dpt_bytes_len);

        align_span(record_span);
        const auto att_len{read_arbitrary<u32>(record_span)};

        align_span(record_span);
        const auto att_bytes_len{att_len * sizeof(checkpoint_att_entry)};
        record.att = gsl::span<const checkpoint_att_entry>{
            reinterpret_cast<const checkpoint_att_entry*>(record_span.data()), att_len};
        record_span = record_span.subspan(att_bytes_len);
    }

    // Decode footer
    record.checksum = read_arbitrary<u32>(record_span);
    const auto size_copy{read_arbitrary<i32>(record_span)};
    if (record.size != size_copy) { return stdx::err{error_t::WAL_SIZE_CORRUPT}; }

    // Verify the checksum matches
    const auto checksum_bytes{original_span.subspan(
        0, static_cast<usize>(record_size) - sizeof(record.checksum) - sizeof(record.size))};
    if (stdx::crc::crc32(checksum_bytes) != record.checksum) {
        return stdx::err{error_t::WAL_CHECKSUM_CORRUPT};
    }
    return record;
}

} // namespace cairn::wal
