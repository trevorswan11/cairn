#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <system_error>

#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/utility.hh>

namespace cairn::tests::helpers {

[[nodiscard]] auto span_from_string(std::string_view str) -> gsl::span<const std::byte>;
[[nodiscard]] auto string_from_span(gsl::span<const std::byte> span) -> std::string_view;

template <std::integral I>
[[nodiscard]] constexpr auto parse_integral(std::string_view str) noexcept -> stdx::option<I> {
    I          out;
    const auto result{std::from_chars(str.begin(), str.end(), out, 10)};
    if (result.ec == std::errc{} && result.ptr == str.end()) { return out; }
    return stdx::none;
}

} // namespace cairn::tests::helpers
