#include "testhelpers/subprocess.hh"

#include <array>
#include <cstdlib>
#include <string>

#include <cairn/config.h>
#include <fmt/base.h>
#include <fmt/format.h>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/diagnostic/error.hh"
#include "testhelpers/argv.hh"

#if CAIRN_WINDOWS
#    include <iterator>
#    include <ranges>

#    define WIN32_LEAN_AND_MEAN
#    include <handleapi.h>
#    include <libloaderapi.h>
#    include <minwinbase.h>
#    include <minwindef.h>
#    include <processenv.h>
#    include <processthreadsapi.h>
#    include <synchapi.h>
#    include <windows.h>
#elif CAIRN_APPLE
#    include <mach-o/dyld.h>
#endif

#if !CAIRN_WINDOWS
#    include <stdlib.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace cairn::tests::helpers {

auto self_exe_path() -> std::string {
    using namespace stdx::size_literals;
    std::array<char, 1_KiB> buffer;
    buffer.fill(0);

#if CAIRN_WINDOWS
    static_assert(MAX_PATH < buffer.size(), "max path exceeds buffer size");
    ::GetModuleFileNameA(nullptr, buffer.data(), buffer.size());
    return buffer.data();
#elif CAIRN_APPLE
    u32 size{sizeof(buffer)};
    ::_NSGetExecutablePath(buffer.data(), &size);
    return buffer.data();
#else
    const auto len{::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1)};
    if (len != -1) { buffer[static_cast<usize>(len)] = '\0'; }
    return buffer.data();
#endif
}

auto spawn_child(const Argv& args) -> result<u32> {
#if CAIRN_WINDOWS
    auto cmd_line{fmt::format(R"("{}")", args[0])};
    for (const auto& arg : args | std::views::drop(1)) {
        if (!arg) { break; }
        fmt::format_to(std::back_inserter(cmd_line), " {}", arg);
    }

    // I don't think you understand how much I hate working with the windows API
    ::STARTUPINFOA si;
    ::ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    ::PROCESS_INFORMATION pi;
    ::ZeroMemory(&pi, sizeof(pi));

    if (!::CreateProcessA(
            nullptr, cmd_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return stdx::err{error::SUBPROCESS_FAILED};
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    ::DWORD exit_code;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return exit_code;
#else
    const auto pid{::fork()};
    if (pid < 0) { return stdx::err{error::SUBPROCESS_FAILED}; }

    if (pid == 0) {
        // Child
        ::execvp(args[0], args.argv());
        std::_Exit(127); // execvp failed
    } else {
        // Parent
        i32 status;
        if (::waitpid(pid, &status, 0) < 0) { return stdx::err{error::SUBPROCESS_FAILED}; }

        if (WIFEXITED(status)) { return WEXITSTATUS(status); }
        if (WIFSIGNALED(status)) { return 128 + WTERMSIG(status); }
    }
    return stdx::err{error::SUBPROCESS_FAILED};
#endif
}

} // namespace cairn::tests::helpers
