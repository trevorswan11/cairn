#include "support/diagnostic/error.hh"

#include <ostream>
#include <string>
#include <string_view>

#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/option.hh>
#include <stdx/utility.hh>

#include "support/style.hh"

namespace cairn {

namespace {

using level_t = diagnostic::level_t;

[[nodiscard]] constexpr auto level_name(level_t level) noexcept -> std::string_view {
    switch (level) {
    case level_t::ERROR:   return "error";
    case level_t::WARNING: return "warning";
    default:               return "";
    }
}

// Returns the level's style for diagnostic printing
[[nodiscard]] constexpr auto level_style(level_t level) noexcept {
    switch (level) {
    case level_t::ERROR:   return style::RED;
    case level_t::WARNING: return style::LIGHT_YELLOW;
    default:               return style::BASE;
    }
}

} // namespace

auto diagnostic::format(std::ostream&                    os,
                        const stdx::option<std::string>& source_path,
                        stdx::option<bool>               in_terminal) const -> std::ostream& {
    const auto tty{in_terminal.value_or(stdx::is_tty())};

    // The source and location play nicely with one another
    if (source_path) {
        const auto& local_style{tty ? style::WHITE_BOLD : style::BASE};
        os << fmt::format(local_style, "{}:", *source_path);
        if (loc_) {
            os << fmt::format(local_style, "{}: ", *loc_);
        } else {
            fmt::print(os, " ");
        }
    }

    // Here the buffer should be "file:loc: " to print level if present
    if (level_) {
        const auto& local_style{tty ? level_style(*level_) : style::BASE};
        const auto  name{level_name(*level_)};
        os << fmt::format(local_style, "{}:", name);
    }

    // The optional message changes position based on source path presence
    if (message_) {
        fmt::print(os, " {}", *message_);
    } else {
        fmt::print(os, " {}", magic_enum::enum_name(err_));
    }
    if (!source_path && loc_) { os << fmt::format(" {}", *loc_); }
    return os;
}

} // namespace cairn
