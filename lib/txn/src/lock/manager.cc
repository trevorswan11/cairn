#include "txn/lock/manager.hh"

#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/error.hh"
#include "txn/id.hh"
#include "txn/lock/types.hh"

namespace cairn::txn::lock {

auto manager::release_all_locks(id_t id) -> void { TODO(id); }

auto manager::is_wounded(id_t id) const noexcept -> bool { TODO(id); }

auto manager::lock_row_shared(id_t id, index_id_t index_id, u64 key_hash) -> result<void> {
    TODO(id, index_id, key_hash);
}

auto manager::lock_row_exclusive(id_t id, index_id_t index_id, u64 key_hash) -> result<void> {
    TODO(id, index_id, key_hash);
}

auto manager::lock_gap_shared(id_t id, index_id_t index_id, u64 key_hash) -> result<void> {
    TODO(id, index_id, key_hash);
}

auto manager::lock_gap_exclusive(id_t id, index_id_t index_id, u64 key_hash) -> result<void> {
    TODO(id, index_id, key_hash);
}

} // namespace cairn::txn::lock
