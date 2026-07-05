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
#    include <unistd.h>
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
    const auto len{readlink("/proc/self/exe", buffer.data(), buffer.size() - 1)};
    if (len != -1) { buffer[static_cast<usize>(len)] = '\0'; }
    return buffer.data();
#endif
}

} // namespace cairn::tests::helpers
