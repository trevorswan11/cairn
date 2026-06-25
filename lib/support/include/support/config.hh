#pragma once

#include <string>
#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/error.hh"

namespace cairn::support {

struct config {
    std::string               table;
    std::string               host;
    std::string               database;
    std::string               username;
    std::string               password;
    stdx::option<std::string> logfile;
    u32                       port;

    [[nodiscard]] static auto parse(std::string_view table, std::string contents) -> result<config>;
};

} // namespace cairn::support
