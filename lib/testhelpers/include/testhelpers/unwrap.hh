#pragma once

#include <print>
#include <string_view>
#include <type_traits>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "testhelpers/internal/safe_assertions.hh"
#include "testhelpers/mt_verifier.hh"

namespace cairn::tests::helpers {

template <typename T>
concept Unwrappable = stdx::Option<std::remove_cvref_t<T>> ||
                      stdx::Result<std::remove_cvref_t<T>> || stdx::OptSize<std::remove_cvref_t<T>>;

// Unpacks the value in the option or result and returns its value if present
template <Unwrappable U>
[[nodiscard]] auto unwrap(U&& u, std::string_view expr, std::string_view file, int line)
    -> decltype(auto) {
    if (!u) { std::println("Unwrap called on disengaged value ({}): {}:{}", expr, file, line); }
    CAIRN_REQUIRE(u);
    return *std::forward<U>(u);
}

#define UNWRAP(expr) ::cairn::tests::helpers::unwrap((expr), #expr, __FILE__, __LINE__)

template <Unwrappable U>
auto unwrap_err(U&& u, std::string_view expr, std::string_view file, int line) -> decltype(auto) {
    if (u) { std::println("Unwraperr called on engaged value ({}): {}:{}", expr, file, line); }

    using T = std::remove_cvref_t<U>;
    if constexpr (stdx::Option<T>) {
        CAIRN_REQUIRE_FALSE(u);
    } else if constexpr (stdx::Result<T>) {
        CAIRN_REQUIRE_FALSE(u);
        return u.error();
    }
}

#define UNWRAP_ERR(expr) ::cairn::tests::helpers::unwrap_err((expr), #expr, __FILE__, __LINE__)

// Thread safe!
template <Unwrappable U>
[[nodiscard]] auto
mt_unwrap(mt_verifier& verifier, U&& u, std::string_view expr, std::string_view file, int line)
    -> decltype(auto) {
    if (!u) {
        std::println("MT_UNWRAP failed ({}): {}:{}", expr, file, line);
        verifier.check(false, expr, file, line);
        VERIFY(false);
    }
    return *std::forward<U>(u);
}

#define MT_UNWRAP(verifier, expr) \
    ::cairn::tests::helpers::mt_unwrap((verifier), (expr), #expr, __FILE__, __LINE__)

// Thread safe!
template <Unwrappable U>
auto mt_unwrap_err(
    mt_verifier& verifier, U&& u, std::string_view expr, std::string_view file, int line)
    -> decltype(auto) {
    if (u) {
        std::println("MT_UNWRAP_ERR failed ({}): {}:{}", expr, file, line);
        verifier.check(false, expr, file, line);
        VERIFY(false);
    }

    using T = std::remove_cvref_t<U>;
    if constexpr (stdx::Option<T>) {
        // Nothing to return
    } else if constexpr (stdx::Result<T>) {
        return u.error();
    }
}

#define MT_UNWRAP_ERR(verifier, expr) \
    ::cairn::tests::helpers::mt_unwrap_err((verifier), (expr), #expr, __FILE__, __LINE__)

} // namespace cairn::tests::helpers
