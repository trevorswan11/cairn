#pragma once

#include <stdx/fixed/vector.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "txn/id.hh"

namespace cairn::txn {

constexpr usize MAX_ACTIVE_TXNS{128};

struct snapshot_t {
    using txn_buf_t = stdx::fixed::vector<id_t, MAX_ACTIVE_TXNS>;

    timestamp_t read_ts;
    id_t        xmin; // Lower bound to search, [xmin, xmax)
    id_t        xmax; // Upper bound to search, [xmin, xmax)

    txn_buf_t active_txns{}; // Sorted

    [[nodiscard]] auto is_active(id_t id) const noexcept -> bool;
};

} // namespace cairn::txn
