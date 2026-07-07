#include "txn/snapshot.hh"

#include <algorithm>

#include "txn/id.hh"

namespace cairn::txn {

auto snapshot_t::is_active(id_t id) const noexcept -> bool {
    if (id < xmin || id >= xmax) { return false; }
    return std::ranges::binary_search(active_txns, id);
}

} // namespace cairn::txn
