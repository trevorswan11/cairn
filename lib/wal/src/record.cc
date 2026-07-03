#include "wal/record.hh"

#include <cstddef>
#include <vector>

#include <gsl/span>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "error.hh"

namespace cairn::wal {

auto log_record::serialize(std::vector<std::byte>& dest) const -> result<void> { TODO(dest); }

auto log_record::deserialize(gsl::span<const std::byte> src) -> result<log_record> { TODO(src); }

} // namespace cairn::wal
