#include "wal/stub.hh"

#include <stdx/types.hh>

namespace cairn::wal {

auto some_really_complicated_work(i32 a, i32 b) noexcept -> i32 { return a + b; }

} // namespace cairn::wal
