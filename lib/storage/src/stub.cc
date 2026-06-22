#include "storage/stub.hh"

#include <stdx/types.hh>

namespace cairn::storage {

auto some_really_complicated_work(i32 a, i32 b) noexcept -> i32 { return a + b; }

} // namespace cairn::storage
