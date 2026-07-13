#pragma once

#include <concepts>
#include <utility>

#include <fmt/base.h>
#include <stdx/types.hh>

namespace cairn {

// Should be zero indexed and only 1-indexed at print time
struct location {
    usize line{0};
    usize column{0};

    constexpr location() noexcept = default;
    constexpr location(usize line, usize column) noexcept : line{line}, column{column} {}

    [[nodiscard]] constexpr auto operator==(const location& other) const noexcept -> bool = default;
};

template <typename T> struct source_info;

template <typename T>
concept Locateable = requires(T t) {
    { source_info<T>::get(t) } -> std::same_as<location>;
};

template <std::integral I1, std::integral I2> struct source_info<std::pair<I1, I2>> {
    static auto get(const std::pair<I1, I2>& p) noexcept -> location {
        return {static_cast<usize>(p.first), static_cast<usize>(p.second)};
    }
};

template <> struct source_info<location> {
    static auto get(const location& loc) -> location { return loc; }
};

} // namespace cairn

template <> struct fmt::formatter<cairn::location> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(const cairn::location& loc, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}:{}", loc.line + 1, loc.column + 1);
    }
};
