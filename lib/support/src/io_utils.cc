#include "support/io_utils.hh"

#include <cerrno>
#include <system_error>

namespace cairn::io_utils {

auto interrupted() noexcept -> bool {
    return std::error_code{errno, std::generic_category()} == std::errc::interrupted;
}

} // namespace cairn::io_utils
