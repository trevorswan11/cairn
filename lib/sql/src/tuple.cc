#include "sql/tuple.hh"

#include <cstddef>
#include <cstring>
#include <ranges>
#include <stdx/variant.hh>
#include <string_view>
#include <utility>

#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "sql/schema.hh"
#include "sql/type.hh"
#include "sql/value.hh"
#include "support/error.hh"

namespace cairn::sql {

auto tuple::serialize(const schema& sch, gsl::span<const value> values) -> result<tuple> {
    if (values.size() != sch.column_count()) {
        return stdx::err{error_t::SQL_VALUE_SCHEMA_COUNT_MISMATCH};
    }

    const usize null_bitmap_size{(sch.column_count() + 7) / 8};
    const auto  fixed_size{sch.fixed_length_size()};

    usize varlen_count{0};
    usize varlen_data_size{0};
    for (usize i = 0; i < sch.column_count(); ++i) {
        if (!sch[i].size()) {
            varlen_count++;
            if (!values[i].is_null()) {
                varlen_data_size += values[i].get_value().as<std::string_view>().size();
            }
        }
    }

    const usize varlen_dir_size{varlen_count * 4}; // offset(u16) + length(u16)
    const usize total_size{null_bitmap_size + fixed_size + varlen_dir_size + varlen_data_size};
    byte_buffer data;
    data.resize(total_size, std::byte{0});

    // Write null bitmap
    for (usize i{0}; i < values.size(); ++i) {
        if (values[i].is_null()) { data[i / 8] |= static_cast<std::byte>(1 << (i % 8)); }
    }

    usize fixed_offset{null_bitmap_size};
    usize varlen_dir_offset{fixed_offset + fixed_size};
    usize varlen_data_offset{varlen_dir_offset + varlen_dir_size};

    for (auto [col, val] : std::views::zip(sch.columns(), values)) {
        const auto val_data{val.get_value()};
        if (auto fixed_col_size_opt{col.size()}) {
            const auto fixed_col_size{*fixed_col_size_opt};
            if (!val.is_null()) {
                val_data.visit(
                    [&](bool b) {
                        const auto v{std::byte{b}};
                        std::memcpy(data.data() + fixed_offset, &v, fixed_col_size);
                    },
                    [&](auto v) { std::memcpy(data.data() + fixed_offset, &v, fixed_col_size); },
                    [](stdx::monostate) { UNREACHABLE("An invalid value slipped through"); },
                    [](std::string_view) { UNREACHABLE("Varchar shouldn't be fixed width"); });
            }
            fixed_offset += fixed_col_size;
        } else {
            if (!val.is_null()) {
                const auto& str{val_data.as<std::string_view>()};
                const u16   len{static_cast<u16>(str.size())};
                const u16   offset{static_cast<u16>(varlen_data_offset)};

                std::memcpy(data.data() + varlen_dir_offset, &offset, 2);
                std::memcpy(data.data() + varlen_dir_offset + 2, &len, 2);
                std::memcpy(data.data() + varlen_data_offset, str.data(), len);
                varlen_data_offset += len;
            } else {
                static constexpr u16 zero{0};
                std::memcpy(data.data() + varlen_dir_offset, &zero, 2);
                std::memcpy(data.data() + varlen_dir_offset + 2, &zero, 2);
            }
            varlen_dir_offset += 4;
        }
    }

    return tuple{std::move(data)};
}

auto tuple::get_value(const schema& sch, usize column_index) const -> result<value> {
    if (column_index >= sch.column_count()) { return stdx::err{error_t::SQL_INVALID_COLUMN_INDEX}; }

    const usize byte_idx{column_index / 8};
    const usize bit_idx{column_index % 8};

    const auto  is_null{(static_cast<u8>(data_[byte_idx]) & (1 << bit_idx)) != 0};
    const auto& col{sch[column_index]};
    if (is_null) { return value::make_null(col.type()); }

    usize null_bitmap_size{(sch.column_count() + 7) / 8};
    usize fixed_offset{null_bitmap_size};
    usize varlen_count_before{0};

    for (usize i = 0; i < column_index; ++i) {
        if (const auto col_size{sch[i].size()}) {
            fixed_offset += *col_size;
        } else {
            varlen_count_before++;
        }
    }

    if (const auto col_size{col.size()}) {
        switch (col.type()) {
        case type::id_t::BOOLEAN: {
            i8 out;
            std::memcpy(&out, data_.data() + fixed_offset, *col_size);
            return value(out != 0);
        }
        case type::id_t::TINYINT: {
            i8 out;
            std::memcpy(&out, data_.data() + fixed_offset, *col_size);
            return value(out);
        }
        case type::id_t::SMALLINT: {
            i16 out;
            std::memcpy(&out, data_.data() + fixed_offset, *col_size);
            return value(out);
        }
        case type::id_t::INTEGER: {
            i32 out;
            std::memcpy(&out, data_.data() + fixed_offset, *col_size);
            return value(out);
        }
        case type::id_t::BIGINT: {
            i64 out;
            std::memcpy(&out, data_.data() + fixed_offset, *col_size);
            return value(out);
        }
        case type::id_t::FLOAT: {
            float out;
            std::memcpy(&out, data_.data() + fixed_offset, *col_size);
            return value(out);
        }
        case type::id_t::DOUBLE: {
            double out;
            std::memcpy(&out, data_.data() + fixed_offset, *col_size);
            return value(out);
        }
        default: return stdx::err{error_t::SQL_UNSUPPORTED_FIXED_TYPE};
        }
    }

    usize varlen_dir_offset{null_bitmap_size + sch.fixed_length_size() + varlen_count_before * 4};
    u16   offset, len;
    std::memcpy(&offset, data_.data() + varlen_dir_offset, 2);
    std::memcpy(&len, data_.data() + varlen_dir_offset + 2, 2);

    std::string_view str{reinterpret_cast<const char*>(data_.data() + offset), len};
    return value(str);
}

auto tuple::deserialize(const schema& sch, gsl::span<value> buf) const -> result<void> {
    if (buf.size() != sch.column_count()) {
        return stdx::err{error_t::SQL_VALUE_SCHEMA_COUNT_MISMATCH};
    }
    for (usize i{0}; i < sch.column_count(); ++i) { buf[i] = TRY(get_value(sch, i)); }
    return {};
}

} // namespace cairn::sql
