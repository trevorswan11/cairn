#include "txn/manager.hh"

#include <mutex>
#include <utility>
#include <vector>

#include <gsl/pointers>
#include <stdx/enum.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/error.hh"
#include "txn/id.hh"
#include "wal/checkpoint/types.hh"
#include "wal/log/manager.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"

namespace cairn::txn {

using namespace stdx::enum_ops;

auto manager::begin_txn() -> id_t {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    const id_t       id{next_txn_id_++};
    active_txns_.emplace(id,
                         att_entry{
                             .txn_id   = id,
                             .state    = att_entry::state_t::ACTIVE,
                             .last_lsn = stdx::none,
                             .read_ts  = global_ts_,
                         });
    return id;
}

auto manager::commit_txn(id_t id, wal::log::manager& manager) -> result<void> {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    auto [found, it]{TRY(find_id_locked(id))};

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
    active_txns_.erase(it);

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
    active_txns_.erase(it);

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

auto manager::snapshot_att() -> std::vector<att_entry> {
    std::unique_lock       lock{mutex_};
    std::vector<att_entry> att;
    att.reserve(active_txns_.size());
    for (const auto& [_, entry] : active_txns_) { att.emplace_back(entry); }
    return att;
}

auto manager::set_next_txn_id(id_t id) noexcept -> void {
    std::scoped_lock lock{mutex_};
    next_txn_id_ = id;
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

auto manager::snapshot_horizon() const noexcept -> timestamp_t {
    std::unique_lock lock{mutex_};
    return snapshot_horizon_locked();
}

auto manager::prune_committed_txns(timestamp_t horizon) noexcept -> void {
    std::unique_lock lock{mutex_};
    prune_committed_txns_locked(horizon);
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

} // namespace cairn::txn
