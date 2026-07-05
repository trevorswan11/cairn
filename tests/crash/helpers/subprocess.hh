#pragma once

#include <string>

namespace cairn::tests::helpers {

// Platform-independent getter
[[nodiscard]] auto self_exe_path() -> std::string;

} // namespace cairn::tests::helpers
