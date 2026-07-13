#pragma once

#include <cerrno>
#include <ios>

#include <stdx/result.hh>

#include "support/diagnostic/error.hh"

#include <type_traits>

namespace cairn::io_utils {

// This might be true when running heavily instrumented coverage builds
[[nodiscard]] auto interrupted() noexcept -> bool;

// Perform a safe operation on a file with interruption checks
template <typename File, typename Fn>
[[nodiscard]] auto run_io(File& file, Fn&& fn) -> result<void> {
    while (true) {
        file.clear();
        errno = 0;
        if constexpr (std::is_same_v<decltype(fn()), result<void>>) {
            TRY(fn());
        } else {
            fn();
        }
        if (!file.fail()) { return {}; }
        if (interrupted()) { continue; }
        return stdx::err{error::IO_ERROR};
    }
}

template <typename File> [[nodiscard]] auto seek_to_beg(File& file) -> result<void> {
    return run_io(file, [&] { file.seekg(0, std::ios::beg); });
}

template <typename File> [[nodiscard]] auto seek_to_end(File& file) -> result<void> {
    return run_io(file, [&] { file.seekg(0, std::ios::end); });
}

template <typename File> [[nodiscard]] auto try_flush(File& file) -> result<void> {
    return run_io(file, [&] { file.flush(); });
}

} // namespace cairn::io_utils
