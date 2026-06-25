#include "support/config.hh"

#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/error.hh"

namespace cairn::support {

namespace {

template <typename Json>
[[nodiscard]] auto parse_value(const Json& json, std::string_view at)
    -> result<nlohmann::json::const_iterator> {
    if (auto it{json.find(at)}; it != json.end()) { return it; }
    return stdx::err{error_t::JSON_MISSING_FIELD};
}

} // namespace

auto config::parse(std::string_view table, std::string contents) -> result<config> {
    stdx::option<nlohmann::json> parsed;
    try {
        parsed.emplace(nlohmann::json::parse(contents, nullptr, true, true));
    } catch (...) { return stdx::err{error_t::JSON_PARSE_ERROR}; }

    try {
        const auto&               database{*TRY(parse_value(*parsed, table))};
        stdx::option<std::string> logfile;
        if (database.contains("logfile")) {
            logfile.emplace(*TRY(parse_value(database, "logfile")));
        }

        return config{
            .table    = std::string{table},
            .host     = *TRY(parse_value(database, "host")),
            .database = *TRY(parse_value(database, "database")),
            .username = *TRY(parse_value(database, "username")),
            .password = *TRY(parse_value(database, "password")),
            .logfile  = std::move(logfile),
            .port     = *TRY(parse_value(database, "port")),
        };
    } catch (...) { return stdx::err{error_t::JSON_TYPE_ERROR}; }
}

} // namespace cairn::support
