#pragma once

#include <string>

#include <gsl/span>
#include <stdx/types.hh>

#include "support/diagnostic/error.hh"
#include "testhelpers/argv.hh"

namespace cairn::tests::helpers {

[[nodiscard]] auto self_exe_path() -> std::string;

// Spawns the child and waits for its termination, returning the exit code
[[nodiscard]] auto spawn_child(const Argv& args) -> result<u32>;

} // namespace cairn::tests::helpers
