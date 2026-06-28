#pragma once

#include <filesystem>
#include <string_view>

#include <stdx/utility.hh>

namespace cairn::tests::helpers {

struct tempfile {
    std::filesystem::path path;

    explicit tempfile(std::string_view tag);
    ~tempfile();
    MAKE_PINNED(tempfile);
};

} // namespace cairn::tests::helpers
