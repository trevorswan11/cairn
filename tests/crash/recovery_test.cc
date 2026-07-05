#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <stdx/option.hh>

#include "helpers/subprocess.hh"

namespace cairn::tests {

TEST_CASE("crash recovery loop") { fmt::println("{}", helpers::self_exe_path()); }

} // namespace cairn::tests
