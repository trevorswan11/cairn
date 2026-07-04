#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>

#include "wal/record.hh"

namespace cairn::tests::helpers {

constexpr std::array redo_bytes{std::byte{0x1}, std::byte{0x2}, std::byte{0x3}};
constexpr std::array undo_bytes{std::byte{0x9}, std::byte{0x8}};

[[nodiscard]] auto write_begin_log(std::vector<std::byte>& buffer) -> wal::record;
[[nodiscard]] auto write_update_log(std::vector<std::byte>& buffer) -> wal::record;
[[nodiscard]] auto write_clear_log(std::vector<std::byte>& buffer) -> wal::record;

// Reads a log from the buffer and checks it against the original before returning
auto read_check_log(const wal::record& original, gsl::span<const std::byte>& src) -> wal::record;
auto records_eq(const wal::record& a, const wal::record& b) noexcept -> void;

} // namespace cairn::tests::helpers
