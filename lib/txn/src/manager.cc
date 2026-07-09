#include "txn/manager.hh"

#include <algorithm>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/enum.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/bplus.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "txn/lock/manager.hh"
#include "txn/lock/types.hh"
#include "txn/read_set.hh"
#include "txn/snapshot.hh"
#include "wal/checkpoint/types.hh"
#include "wal/log/manager.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"

namespace cairn::txn {

using namespace stdx::enum_ops;

auto manager::begin_txn(isolation_level_t level) -> id_t {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    const id_t       id{next_txn_id_++};

    {
        PROFILE_SCOPE("capture currently active txns");
        bool  added_new{false};
        auto& active{active_txns_at_start_[id]};

        for (const auto& [tid, _] : active_txns_) {
            if (active.size() < active.capacity()) {
                active.emplace_back(tid);
                added_new = true;
            } else {
                break;
            }
        }

        // Only sort if there was an addition to save compute
        if (added_new) { std::ranges::sort(active); }
    }

    active_txns_.emplace(id,
                         att_entry{
                             .txn_id   = id,
                             .state    = att_entry::state_t::ACTIVE,
                             .last_lsn = stdx::none,
                             .read_ts  = global_ts_,
                         });

    switch (level) {
    case isolation_level_t::SNAPSHOT:        break;
    case isolation_level_t::SERIALIZABLE:    read_sets_.emplace(id, tree_read_set_map_t{}); break;
    case isolation_level_t::READ_COMMITTED:  break;
    case isolation_level_t::REPEATABLE_READ: break;
    }
    isolation_map_.emplace(id, level);

    return id;
}

auto manager::commit_txn(id_t id, wal::log::manager& manager) -> result<void> {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    if (lock_manager_ && lock_manager_->is_wounded(id)) {
        return stdx::err{error_t::TXN_DEADLOCK_DETECTED};
    }

    auto [found, it]{TRY(find_id_locked(id))};
    if (auto rs_it{read_sets_.find(id)}; rs_it != read_sets_.end()) {
        const auto read_ts{found->read_ts.value_or(timestamp_t{global_ts_})};
        const auto get_commit_ts = [&](id_t writer_id) -> stdx::option<timestamp_t> {
            if (auto ct_it{committed_txns_.find(writer_id)}; ct_it != committed_txns_.end()) {
                return ct_it->second;
            }
            return stdx::none;
        };

        bool conflict{false};
        for (const auto& [_, read_set] : rs_it->second) {
            if (TRY(read_set->check_conflict(id, read_ts, get_commit_ts))) {
                conflict = true;
                break;
            }
        }

        if (conflict) { return stdx::err{error_t::TXN_SERIALIZATION_FAILURE}; }
    }

    if (found->last_lsn) {
        wal::log::record rec;
        rec.txn_id   = id;
        rec.type     = wal::log::record_type::COMMIT;
        rec.prev_lsn = found->last_lsn;

        auto lsn{TRY(manager.append_record(rec))};
        TRY(manager.flush(lsn));
    }

    const timestamp_t commit_ts{++global_ts_};
    committed_txns_.emplace(id, commit_ts);

    if (lock_manager_) { lock_manager_->release_all_locks(id); }
    active_txns_.erase(it);
    active_txns_at_start_.erase(id);
    read_sets_.erase(id);
    isolation_map_.erase(id);

    const auto horizon{snapshot_horizon_locked()};
    prune_committed_txns_locked(horizon);
    return {};
}

auto manager::abort_txn(id_t id, wal::log::manager& manager) -> result<void> {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    auto [found, it]{TRY(find_id_locked(id))};

    if (found->last_lsn) {
        wal::log::record rec;
        rec.txn_id   = id;
        rec.type     = wal::log::record_type::ABORT;
        rec.prev_lsn = found->last_lsn;

        auto lsn{TRY(manager.append_record(rec))};
        TRY(manager.flush(lsn));
    }

    if (lock_manager_) { lock_manager_->release_all_locks(id); }
    active_txns_.erase(it);
    active_txns_at_start_.erase(id);
    read_sets_.erase(id);
    isolation_map_.erase(id);

    const auto horizon{snapshot_horizon_locked()};
    prune_committed_txns_locked(horizon);
    return {};
}

auto manager::update_txn_lsn(id_t id, wal::log::seq_num lsn) -> result<void> {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    auto [found, _]{TRY(find_id_locked(id))};
    found->last_lsn = lsn;
    return {};
}

auto manager::snapshot_att(std::vector<att_entry>& buf) -> void {
    std::unique_lock lock{mutex_};
    buf.clear();
    buf.reserve(active_txns_.size());
    for (const auto& [_, entry] : active_txns_) { buf.emplace_back(entry); }
}

auto manager::set_next_txn_id(id_t id) noexcept -> void {
    std::scoped_lock lock{mutex_};
    next_txn_id_ = id;
}

auto manager::set_lock_manager(stdx::option<lock::manager&> lock_manager) noexcept -> void {
    std::scoped_lock lock{mutex_};
    lock_manager_ = lock_manager;
}

auto manager::get_read_timestamp(id_t id) const -> result<timestamp_t> {
    std::unique_lock lock{mutex_};
    if (auto it{active_txns_.find(id)}; it != active_txns_.end() && it->second.read_ts) {
        return *it->second.read_ts;
    }
    return stdx::err{error_t::TXN_NOT_FOUND};
}

auto manager::get_commit_timestamp(id_t id) const -> result<stdx::option<timestamp_t>> {
    std::unique_lock lock{mutex_};
    if (auto it{committed_txns_.find(id)}; it != committed_txns_.end()) { return it->second; }
    if (active_txns_.contains(id)) { return stdx::err{error_t::TXN_NOT_FOUND}; }
    if (id != INVALID_TXN_ID && id < next_txn_id_) { return stdx::none; }
    return stdx::err{error_t::TXN_NOT_FOUND};
}

auto manager::get_isolation_level(id_t id) const -> result<isolation_level_t> {
    std::unique_lock lock{mutex_};
    if (auto it{isolation_map_.find(id)}; it != isolation_map_.end()) { return it->second; }
    return stdx::err{error_t::TXN_NOT_FOUND};
}

auto manager::committed_before_horizon(id_t id, timestamp_t horizon) const noexcept -> bool {
    std::unique_lock lock{mutex_};
    ASSERT(id != INVALID_TXN_ID, "The id should be validated before calling this");

    if (active_txns_.contains(id)) { return false; }
    if (id >= next_txn_id_) { return false; }
    if (auto it{committed_txns_.find(id)}; it != committed_txns_.end()) {
        return it->second <= horizon;
    }

    // If it's committed but not in committed_txns_,
    // it must have committed and been pruned, which means its commit timestamp was < horizon.
    return true;
}

auto manager::snapshot_horizon() const noexcept -> timestamp_t {
    std::unique_lock lock{mutex_};
    return snapshot_horizon_locked();
}

auto manager::prune_committed_txns(timestamp_t horizon) noexcept -> void {
    std::unique_lock lock{mutex_};
    prune_committed_txns_locked(horizon);
}

auto manager::acquire_snapshot(id_t id) const -> result<snapshot_t> {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};

    auto txn_it{active_txns_.find(id)};
    if (txn_it == active_txns_.end()) { return stdx::err{error_t::TXN_NOT_FOUND}; }

    auto level{isolation_level_t::SNAPSHOT};
    if (auto it{isolation_map_.find(id)}; it != isolation_map_.end()) { level = it->second; }

    if (level == isolation_level_t::READ_COMMITTED) { return acquire_snapshot_locked(); }

    const auto read_ts{txn_it->second.read_ts.value_or(global_ts_)};

    snapshot_t::txn_buf_t active;
    if (auto buf_it{active_txns_at_start_.find(id)}; buf_it != active_txns_at_start_.end()) {
        active = buf_it->second;
    }

    return snapshot_t{
        .read_ts     = read_ts,
        .xmin        = active.empty() ? id : active.front(),
        .xmax        = id,
        .active_txns = std::move(active),
    };
}

auto manager::acquire_snapshot() const -> snapshot_t {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    return acquire_snapshot_locked();
}

auto manager::is_visible(const snapshot_t&  snap,
                         id_t               reader_id,
                         stdx::option<id_t> version_id,
                         bool               is_timestamp) const -> bool {
    // Transactions can always see their own writes
    const auto vid{version_id.value_or(INVALID_TXN_ID)};
    if (vid == reader_id) { return true; }
    if (is_timestamp) { return static_cast<timestamp_t>(vid) <= snap.read_ts; }
    if (vid >= snap.xmax) { return false; }
    return !snap.is_active(vid);
}

auto manager::get_or_create_read_set(id_t                      id,
                                     storage::tree_id_t        tree_id,
                                     const read_set_factory_t& factory) const
    -> stdx::option<read_set_t&> {
    std::unique_lock lock{mutex_};
    auto             it{read_sets_.find(id)};
    if (it == read_sets_.end()) { return stdx::none; }

    auto& map{it->second};
    auto  map_it{map.find(tree_id)};
    if (map_it == map.end()) {
        auto [inserted_it, _]{map.emplace(tree_id, factory())};
        return inserted_it->second.get();
    }
    return map_it->second.get();
}

auto manager::lock_row_shared(id_t id, storage::tree_id_t tree_id, lock::key_hash_t key_hash) const
    -> result<void> {
    if (lock_manager_) {
        return lock_manager_->lock_row_shared(id, std::to_underlying(tree_id), key_hash);
    }
    return {};
}

auto manager::lock_row_exclusive(id_t               id,
                                 storage::tree_id_t tree_id,
                                 lock::key_hash_t   key_hash) const -> result<void> {
    if (lock_manager_) {
        return lock_manager_->lock_row_exclusive(id, std::to_underlying(tree_id), key_hash);
    }
    return {};
}

auto manager::release_shared_locks(id_t id) const -> void {
    if (lock_manager_) { lock_manager_->release_shared_locks(id); }
}

auto manager::find_id_locked(id_t id) noexcept -> found_id_res_t {
    if (auto it{active_txns_.find(id)}; it != active_txns_.end()) {
        return std::make_pair(gsl::not_null{&it->second}, it);
    }
    return stdx::err{error_t::TXN_NOT_FOUND};
}

auto manager::prune_committed_txns_locked(timestamp_t horizon) noexcept -> void {
    std::erase_if(committed_txns_, [horizon](const auto& item) { return item.second < horizon; });
}

auto manager::snapshot_horizon_locked() const noexcept -> timestamp_t {
    timestamp_t min_ts{global_ts_};
    for (const auto& [_, entry] : active_txns_) {
        if (entry.read_ts && *entry.read_ts < min_ts) { min_ts = *entry.read_ts; }
    }
    return min_ts;
}

auto manager::acquire_snapshot_locked() const -> snapshot_t {
    snapshot_t::txn_buf_t active;
    for (const auto& [tid, _] : active_txns_ | std::views::take(active.capacity() - 1)) {
        active.emplace_back(tid);
    }
    std::ranges::sort(active);

    return snapshot_t{
        .read_ts     = global_ts_,
        .xmin        = active.empty() ? next_txn_id_ : active.front(),
        .xmax        = next_txn_id_,
        .active_txns = std::move(active),
    };
}

} // namespace cairn::txn
