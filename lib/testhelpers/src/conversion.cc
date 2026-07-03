#include <cstddef>
#include <string_view>

#include <gsl/span>
#include <stdx/utility.hh>

namespace cairn::tests::helpers {

auto span_from_string(std::string_view str) -> gsl::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(str.data()), str.size()};
}

auto string_from_span(gsl::span<const std::byte> span) -> std::string_view {
    return {reinterpret_cast<const char*>(span.data()), span.size()};
}

} // namespace cairn::tests::helpers
