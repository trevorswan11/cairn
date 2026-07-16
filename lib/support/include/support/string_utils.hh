#pragma once

#include <ranges>
#include <string_view>

#include <stdx/hash.hh>

namespace cairn::string_utils {

[[nodiscard]] constexpr auto to_lower(char c) noexcept -> char {
    if (c >= 'A' && c <= 'Z') { return c + ('a' - 'A'); }
    return c;
}

// Case insensitive string equality
struct iequals {
    [[nodiscard]] static constexpr auto operator()(std::string_view lhs,
                                                   std::string_view rhs) noexcept -> bool {
        if (lhs.size() != rhs.size()) { return false; }
        for (const auto [l, r] : std::views::zip(lhs, rhs)) {
            if (to_lower(l) != to_lower(r)) { return false; }
        }
        return true;
    }
};

// Case insensitive string compile-time hashing
struct ihash {
    [[nodiscard]] static constexpr auto operator()(std::string_view sv) noexcept {
        return stdx::crc::crc32(sv | std::views::transform(to_lower));
    }
};

} // namespace cairn::string_utils
