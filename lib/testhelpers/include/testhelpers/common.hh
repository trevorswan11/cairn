#pragma once

#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

namespace cairn::tests::helpers {

template <typename T>
concept Unwrappable = stdx::Option<std::remove_cvref_t<T>> ||
                      stdx::Result<std::remove_cvref_t<T>> || stdx::OptSize<std::remove_cvref_t<T>>;

// Unpacks the value in the option or result and returns its value if present
template <Unwrappable U> [[nodiscard]] auto unwrap(U&& u) -> decltype(auto) {
    REQUIRE(u);
    return *u;
}

// Unpacks the value in the option or result and returns its value if present and equal to expected
template <Unwrappable U, typename E>
[[nodiscard]] auto unwrap(U&& u, E&& expected_value) -> decltype(auto) {
    REQUIRE(u);
    REQUIRE(*u == std::forward<E>(expected_value));
    return *u;
}

template <Unwrappable U> auto unwrap_err(U&& u) -> decltype(auto) {
    using T = std::remove_cvref_t<U>;
    if constexpr (stdx::Option<T>) {
        REQUIRE_FALSE(u);
    } else if constexpr (stdx::Result<T>) {
        REQUIRE_FALSE(u);
        return u.error();
    }
}

[[nodiscard]] auto some_really_complicated_work(i32 a, i32 b) noexcept -> i32;

} // namespace cairn::tests::helpers
