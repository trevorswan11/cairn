#include "helpers/subprocess.hh"

#include <array>
#include <string>

#include <stdx/memory.hh>
#include <stdx/types.hh>

#include <cairn/config.h>

#if CAIRN_WINDOWS
#    include <libloaderapi.h>
#    include <minwindef.h>
#elif CAIRN_APPLE
#    include <mach-o/dyld.h>
#else
#endif

namespace cairn::tests::helpers {

auto self_exe_path() -> std::string {
    using namespace stdx::size_literals;
    std::array<char, 1_KiB> buffer;
    buffer.fill(0);

#if CAIRN_WINDOWS
    ::GetModuleFileNameA(nullptr, buffer.data(), buffer.size());
    return buffer.data();
#elif CAIRN_APPLE
    u32 size{sizeof(buffer)};
    ::_NSGetExecutablePath(buffer.data(), &size);
    return buffer.data();
#else
#endif
}

} // namespace cairn::tests::helpers
