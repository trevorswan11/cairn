#include <sstream>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/diagnostic/error.hh"
#include "support/diagnostic/location.hh"

namespace cairn {

namespace {

struct a_something {};
struct b_something {};

} // namespace

template <> struct source_info<a_something> {
    static auto get(const a_something&) noexcept -> location { return {0, 42}; }
};

template <> struct source_info<b_something> {
    static auto get(const b_something&) noexcept -> location { return {42, 0}; }
};

namespace tests {

namespace {

auto check_diagnostic_format(const diagnostic&              d,
                             std::string_view               expected,
                             stdx::option<std::string_view> file = stdx::none) -> void {
    std::stringstream ss;
    d.format(ss, file, false);
    CHECK(expected == ss.view());
}

} // namespace

TEST_CASE("Location and error only") {
    a_something l;
    diagnostic  d{error::IO_ERROR, l};
    check_diagnostic_format(d, "error: SAD 1:43");
}

TEST_CASE("Custom locateable") {
    a_something l;
    diagnostic  d{"message", error::IO_ERROR, l};
    d.set_level(diagnostic::level::WARNING);
    check_diagnostic_format(d, "warning: message 1:43");
}

TEST_CASE("Pair locateable") {
    diagnostic d{"message", error::IO_ERROR, std::pair{0, 0}};
    check_diagnostic_format(d, "error: message 1:1");
}

TEST_CASE("Error messages with associated files") {
    diagnostic d{"message", error::IO_ERROR};
    check_diagnostic_format(d, "foo.sql: error: message", "foo.sql");
}

TEST_CASE("Locateable Error messages with associated files") {
    a_something l;
    diagnostic  d{"message", error::IO_ERROR, l};
    check_diagnostic_format(d, "foo.sql:1:43: error: message", "foo.sql");
}

TEST_CASE("Move constructor with new error") {
    a_something l;
    diagnostic  d1{"message", error::IO_ERROR, l};
    diagnostic  d2{std::move(d1), error::SQL_EOF};
    check_diagnostic_format(d2, "error: message 1:43");
}

TEST_CASE("Move constructor with new location") {
    a_something l;
    diagnostic  d1{"message", error::IO_ERROR, l};

    b_something e;
    diagnostic  d2{std::move(d1), e};
    check_diagnostic_format(d2, "error: message 43:1");
}

} // namespace tests

} // namespace cairn
