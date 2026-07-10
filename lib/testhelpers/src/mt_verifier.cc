#include "testhelpers/mt_verifier.hh"

#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/base.h>
#include <fmt/format.h>

namespace cairn::tests::helpers {

auto mt_verifier::add_failure(std::string msg) -> void {
    std::scoped_lock lock{mutex_};
    failures_.emplace_back(std::move(msg));
}

auto mt_verifier::check(bool condition, std::string_view expr, std::string_view file, int line)
    -> void {
    if (!condition) { add_failure(fmt::format("{} failed at {}:{}", expr, file, line)); }
}

auto mt_verifier::dump_if_error() -> bool {
    const auto has_errors{!failures_.empty()};
    for (const auto& failure : failures_) { fmt::println("\t{}", failure); }
    failures_.clear();
    return has_errors;
}

} // namespace cairn::tests::helpers
