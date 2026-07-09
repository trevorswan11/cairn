#pragma once

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/bplus.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "txn/lock/types.hh"
#include "txn/read_set.hh"
#include "txn/snapshot.hh"
#include "wal/checkpoint/types.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::txn {

namespace lock { class manager; } // namespace lock

class manager {
  public:
    using att_entry          = wal::checkpoint::att_entry;
    using active_txn_map_t   = ankerl::unordered_dense::map<id_t, att_entry, id_hash_t>;
    using commited_txn_map_t = ankerl::unordered_dense::map<id_t, timestamp_t, id_hash_t>;
    using found_id_res_t = result<std::pair<gsl::not_null<att_entry*>, active_txn_map_t::iterator>>;
    using snapshot_txn_map_t = ankerl::unordered_dense::map<id_t, snapshot_t::txn_buf_t, id_hash_t>;

    using tree_read_set_map_t =
        ankerl::unordered_dense::map<storage::tree_id_t, stdx::box<read_set_t>>;
    using read_set_factory_t = std::function<stdx::box<read_set_t>()>;
    using read_set_map_t     = ankerl::unordered_dense::map<id_t, tree_read_set_map_t, id_hash_t>;

    using isolation_map_t = ankerl::unordered_dense::map<id_t, isolation_level_t, id_hash_t>;

  public:
    [[nodiscard]] auto begin_txn(isolation_level_t level = isolation_level_t::SNAPSHOT) -> id_t;
    [[nodiscard]] auto commit_txn(id_t id, wal::log::manager& manager) -> result<void>;
    [[nodiscard]] auto abort_txn(id_t id, wal::log::manager& manager) -> result<void>;
    [[nodiscard]] auto update_txn_lsn(id_t id, wal::log::seq_num lsn) -> result<void>;
    auto               snapshot_att(std::vector<att_entry>& buf) -> void;

    auto set_next_txn_id(id_t id) noexcept -> void;
    auto set_lock_manager(stdx::option<lock::manager&> lock_manager) noexcept -> void;

    // Retrieve the read timestamp of an active transaction
    [[nodiscard]] auto get_read_timestamp(id_t id) const -> result<timestamp_t>;

    // Retrieve the commit timestamp of a recently committed transaction
    [[nodiscard]] auto get_commit_timestamp(id_t id) const -> result<stdx::option<timestamp_t>>;
    [[nodiscard]] auto get_isolation_level(id_t id) const -> result<isolation_level_t>;

    // Calculate the minimum read timestamp of all active transactions
    [[nodiscard]] auto snapshot_horizon() const noexcept -> timestamp_t;

    // Clean up committed transaction history older than the horizon
    auto prune_committed_txns(timestamp_t horizon) noexcept -> void;

    // Attempts to retrieve the snapshot of the provided transaction
    [[nodiscard]] auto acquire_snapshot(id_t id) const -> result<snapshot_t>;

    // Creates a snapshot of the current active transactions
    [[nodiscard]] auto acquire_snapshot() const -> snapshot_t;
    [[nodiscard]] auto is_visible(const snapshot_t&  snap,
                                  id_t               reader_id,
                                  stdx::option<id_t> version_id,
                                  bool               is_timestamp) const -> bool;

    [[nodiscard]] auto get_or_create_read_set(id_t                      id,
                                              storage::tree_id_t        tree_id,
                                              const read_set_factory_t& factory) const
        -> stdx::option<read_set_t&>;

    [[nodiscard]] auto lock_row_shared(id_t               id,
                                       storage::tree_id_t tree_id,
                                       lock::key_hash_t   key_hash) const -> result<void>;
    [[nodiscard]] auto lock_row_exclusive(id_t               id,
                                          storage::tree_id_t tree_id,
                                          lock::key_hash_t   key_hash) const -> result<void>;
    auto               release_shared_locks(id_t id) const -> void;

  private:
    [[nodiscard]] auto find_id_locked(id_t id) noexcept -> found_id_res_t;
    auto               prune_committed_txns_locked(timestamp_t horizon) noexcept -> void;
    [[nodiscard]] auto snapshot_horizon_locked() const noexcept -> timestamp_t;
    [[nodiscard]] auto acquire_snapshot_locked() const -> snapshot_t;

  private:
    mutable std::mutex mutex_;
    id_t               next_txn_id_{1};
    timestamp_t        global_ts_{0};

    active_txn_map_t   active_txns_;
    commited_txn_map_t committed_txns_;
    snapshot_txn_map_t active_txns_at_start_;
    isolation_map_t    isolation_map_;

    stdx::option<lock::manager&> lock_manager_;

    mutable read_set_map_t read_sets_;
};

class shared_lock_guard_t {
  public:
    shared_lock_guard_t(const manager& mgr, id_t txn_id, bool active) noexcept
        : mgr_{mgr}, txn_id_{txn_id}, active_{active} {}

    ~shared_lock_guard_t() {
        if (active_) { mgr_.release_shared_locks(txn_id_); }
    }

    shared_lock_guard_t(const shared_lock_guard_t&)                    = delete;
    auto operator=(const shared_lock_guard_t&) -> shared_lock_guard_t& = delete;

    shared_lock_guard_t(shared_lock_guard_t&& other) noexcept
        : mgr_{other.mgr_}, txn_id_{other.txn_id_}, active_{other.active_} {
        other.active_ = false;
    }
    auto operator=(shared_lock_guard_t&&) -> shared_lock_guard_t& = delete;

  private:
    const manager& mgr_;
    id_t           txn_id_;
    bool           active_;
};

} // namespace cairn::txn
