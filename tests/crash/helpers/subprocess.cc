#include "helpers/subprocess.hh"

#include <array>
#include <string>

#include <cairn/config.h>

#if CAIRN_WINDOWS
#    include <libloaderapi.h>
#    include <minwindef.h>
#elif CAIRN_APPLE
#else
#endif

namespace cairn::tests::helpers {

auto self_exe_path() -> std::string {
#if CAIRN_WINDOWS
    std::array<char, MAX_PATH> buffer;
    ::GetModuleFileNameA(nullptr, buffer.data(), buffer.size());
    return buffer.data();
#elif CAIRN_APPLE
#else
#endif
}

} // namespace cairn::tests::helpers
