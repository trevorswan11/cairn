#include "wal/log_reader.hh"

#include <filesystem>

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "error.hh"
#include "wal/log_record.hh"

namespace cairn::wal {

log_reader::log_reader(std::filesystem::path path) { TODO(path); }

auto log_reader::has_next() -> bool { TODO(); }

auto log_reader::next() -> result<log_record> { TODO(); }

auto log_reader::has_prev() -> bool { TODO(); }

auto log_reader::prev() -> result<log_record> { TODO(); }

auto log_reader::seek_to_start() -> result<void> { TODO(); }

auto log_reader::seek_to_end() -> result<void> { TODO(); }

} // namespace cairn::wal
