#include "support/crash/injection.hh"

#include <atomic>
#include <cstdlib>

#include <fmt/base.h>
#include <fmt/std.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/assert.hh>
#include <stdx/types.hh>

namespace cairn::crash {

namespace {

constinit std::atomic<bool> initialized{false};
constinit std::atomic<i32>  boundary_limit{0};
constinit std::atomic<i32>  boundaries{0};

[[noreturn]] auto bail(boundary_t boundary) -> void {
    fmt::println("[CRASH INJECTION] Bailing at boundary: {} (limit: {})",
                 magic_enum::enum_name(boundary),
                 boundary_limit);
    std::_Exit(1);
}

} // namespace

auto initialize() noexcept -> void { initialized.store(true); }

auto configure(i32 limit) noexcept -> void {
    ASSERT(initialized, "configure called prior to initialization");
    boundary_limit.store(limit);
}

auto test_boundary(boundary_t boundary) noexcept -> void {
    if (!initialized) [[likely]] { return; }
    if (++boundaries >= boundary_limit) { bail(boundary); }
}

} // namespace cairn::crash
