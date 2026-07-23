#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "support/indent.hh"

namespace cairn::tests {

using namespace cairn::support;
using guard_t = indent_t::guard_t;

TEST_CASE("Indents over time") {
    indent_t indent;

    SECTION("Empty indent") { CHECK(indent.current_branch() == ""); }

    SECTION("Single non-last") {
        const guard_t g{indent, false};
        CHECK(indent.current_branch() == symbols::T_BRANCH);
    }

    SECTION("Single last") {
        const guard_t g{indent, true};
        CHECK(indent.current_branch() == symbols::L_BRANCH);
    }

    SECTION("Nested levels") {
        const guard_t g1{indent, false};
        {
            const guard_t g2{indent, true};
            CHECK(indent.current_branch() ==
                  fmt::format("{}{}", symbols::VERT_BAR, symbols::L_BRANCH));
        }
    }

    SECTION("Nested levels") {
        const guard_t g1{indent, true};
        const guard_t g2{indent, true};
        const guard_t g3{indent, false};
        CHECK(indent.current_branch() ==
              fmt::format("{}{}{}", symbols::EMPTY, symbols::EMPTY, symbols::T_BRANCH));
    }
}

} // namespace cairn::tests
