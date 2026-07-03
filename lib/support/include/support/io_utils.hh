#pragma once

#include <ios>

#include <stdx/result.hh>

#include "support/error.hh"

namespace cairn::io_utils {

// This might be true when running heavily instrumented coverage builds
[[nodiscard]] auto interrupted() noexcept -> bool;

template <typename File> [[nodiscard]] auto seek_to_beg(File& file) -> result<void> {
    file.seekg(0, std::ios::beg);
    if (file.fail()) { return stdx::err{error_t::IO_ERROR}; }
    return {};
}

template <typename File> [[nodiscard]] auto seek_to_end(File& file) -> result<void> {
    file.seekg(0, std::ios::end);
    if (file.fail()) { return stdx::err{error_t::IO_ERROR}; }
    return {};
}

} // namespace cairn::io_utils
