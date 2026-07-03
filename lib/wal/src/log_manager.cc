#include "wal/log_manager.hh"

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/error.hh"
#include "wal/log_record.hh"

namespace cairn::wal {

log_manager::~log_manager() { TODO(); }

auto log_manager::append_record(log_record& record) -> result<lsn_t> { TODO(record); }

auto log_manager::flush(lsn_t lsn) -> result<void> { TODO(lsn); }

auto log_manager::flush_loop() -> void { TODO(); }

auto log_manager::trigger_buffer_swap() -> void { TODO(); }

} // namespace cairn::wal
