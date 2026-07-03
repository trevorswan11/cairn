#pragma once

#include <cstddef>
#include <string_view>

#include <gsl/span>
#include <stdx/utility.hh>

namespace cairn::tests::helpers {

[[nodiscard]] auto span_from_string(std::string_view str) -> gsl::span<const std::byte>;
[[nodiscard]] auto string_from_span(gsl::span<const std::byte> span) -> std::string_view;

} // namespace cairn::tests::helpers
