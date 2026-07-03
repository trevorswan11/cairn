#pragma once

#include <stdx/memory.hh>

#include "support/config.hh"

namespace spdlog { class logger; } // namespace spdlog

namespace cairn::support {

using logger_t = stdx::rc<spdlog::logger>;

// If the config did not specify a logger all writes are no-ops
[[nodiscard]] auto initialize_logger(const config& cfg) -> logger_t;

} // namespace cairn::support
