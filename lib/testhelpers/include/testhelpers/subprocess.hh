#pragma once

#include <string>
#include <string_view>

#include <gsl/span>
#include <stdx/types.hh>

#include "support/error.hh"
#include "testhelpers/argv.hh"

namespace cairn::tests::helpers {

[[nodiscard]] auto self_exe_path() -> std::string;

auto set_env(const std::string& name, const std::string& value) -> void;
auto get_env(const std::string& name) -> std::string_view;

// Spawns the child and waits for its termination, returning the exit code
[[nodiscard]] auto spawn_child(const Argv& args) -> result<u32>;

} // namespace cairn::tests::helpers
