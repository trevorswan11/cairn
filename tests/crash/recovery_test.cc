#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>

#include "testhelpers/argv.hh"
#include "testhelpers/subprocess.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

TEST_CASE("crash recovery loop", "[.][crash]") { fmt::println("Hello from test subset"); }

TEST_CASE("crash recovery entrypoint") {
    const auto          self_exe{helpers::self_exe_path()};
    const helpers::Argv args{self_exe, "[crash]"};
    CHECK(helpers::unwrap(helpers::spawn_child(args)) == 0);
}

} // namespace cairn::tests
