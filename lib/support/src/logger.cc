#include "support/logger.hh"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <stdx/memory.hh>

#include "support/config.hh"

namespace cairn::support {

[[nodiscard]] auto initialize_logger(const support::config& cfg) -> logger_t {
    if (cfg.logfile) {
        auto file_sink{stdx::make_rc<spdlog::sinks::basic_file_sink_mt>(*cfg.logfile, true)};
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v");
        return stdx::make_rc<spdlog::logger>("cairn_logger", file_sink);
    }

    auto null_sink{stdx::make_rc<spdlog::sinks::null_sink_mt>()};
    return stdx::make_rc<spdlog::logger>("cairn_logger", null_sink);
}

} // namespace cairn::support
