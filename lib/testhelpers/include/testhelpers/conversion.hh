#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <system_error>

#include <gsl/span>
#include <stdx/utility.hh>

#include "testhelpers/internal/safe_assertions.hh"

namespace cairn::tests::helpers {

[[nodiscard]] auto span_from_string(std::string_view str) -> gsl::span<const std::byte>;
[[nodiscard]] auto string_from_span(gsl::span<const std::byte> span) -> std::string_view;

template <std::integral I>
[[nodiscard]] constexpr auto parse_integral(std::string_view str) noexcept -> I {
    I          out;
    const auto result{std::from_chars(str.begin(), str.end(), out, 10)};
    CAIRN_REQUIRE(result.ec == std::errc{});
    CAIRN_REQUIRE(result.ptr == str.end());
    return out;
}

} // namespace cairn::tests::helpers
