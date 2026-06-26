#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

#include <stdx/utility.hh>

namespace cairn::tests::helpers {

struct tempfile {
    std::filesystem::path path;

    explicit tempfile(std::string_view tag);
    MAKE_PINNED(tempfile);
    ~tempfile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace cairn::tests::helpers
