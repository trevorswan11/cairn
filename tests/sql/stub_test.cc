#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>

#include "testhelpers/common.hh"

namespace cairn::tests::sql {

TEST_CASE("Stub") {
    stdx::option<i32> a{1};
    CHECK(helpers::unwrap(a) == 1);
}

} // namespace cairn::tests::sql
