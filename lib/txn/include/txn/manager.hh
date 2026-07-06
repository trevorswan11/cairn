#pragma once

#include <mutex>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/error.hh"
#include "txn/id.hh"
#include "wal/checkpoint/types.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::txn {

class manager {
  public:
    using att_entry          = wal::checkpoint::att_entry;
    using active_txn_map_t   = ankerl::unordered_dense::map<id_t, att_entry, id_hash_t>;
    using commited_txn_map_t = ankerl::unordered_dense::map<id_t, timestamp_t, id_hash_t>;

  public:
    [[nodiscard]] auto begin_txn() -> id_t;
    [[nodiscard]] auto commit_txn(id_t id, wal::log::manager& manager) -> result<void>;
    [[nodiscard]] auto abort_txn(id_t id, wal::log::manager& manager) -> result<void>;
    [[nodiscard]] auto update_txn_lsn(id_t id, wal::log::seq_num lsn) -> result<void>;
    [[nodiscard]] auto snapshot_att() -> std::vector<att_entry>;
    auto               set_next_txn_id(id_t id) noexcept -> void;

    // Retrieve the read timestamp of an active transaction
    [[nodiscard]] auto get_read_timestamp(id_t id) const -> result<timestamp_t>;

    // Retrieve the commit timestamp of a recently committed transaction
    [[nodiscard]] auto get_commit_timestamp(id_t id) const -> result<stdx::option<timestamp_t>>;

    // Calculate the minimum read timestamp of all active transactions
    [[nodiscard]] auto snapshot_horizon() const noexcept -> timestamp_t;

    // Clean up committed transaction history older than the horizon
    auto prune_committed_txns(timestamp_t horizon) noexcept -> void;

  private:
    [[nodiscard]] auto find_id(id_t id) noexcept
        -> result<std::pair<gsl::not_null<att_entry*>, active_txn_map_t::iterator>>;

    auto prune_committed_txns_locked(timestamp_t horizon) noexcept -> void;     // Must hold lock
    [[nodiscard]] auto snapshot_horizon_locked() const noexcept -> timestamp_t; // Must hold lock

  private:
    mutable std::mutex mutex_;
    id_t               next_txn_id_{1};
    timestamp_t        global_ts_{0};

    active_txn_map_t   active_txns_;
    commited_txn_map_t committed_txns_;
};

} // namespace cairn::txn
