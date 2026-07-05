#include "txn/manager.hh"

#include <mutex>
#include <vector>

#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/error.hh"
#include "txn/id.hh"
#include "wal/log_manager.hh"
#include "wal/log_record.hh"
#include "wal/log_sequence_number.hh"

namespace cairn::txn {

auto manager::begin_txn() -> id_t {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    const id_t       id{next_txn_id_++};
    active_txns_.emplace_back(id, wal::att_state_t::ACTIVE, stdx::none);
    return id;
}

auto manager::commit_txn(id_t id, wal::log_manager& manager) -> result<void> {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    auto             it{TRY(find_id(id))};

    if (it->last_lsn) {
        wal::log_record rec;
        rec.txn_id   = id;
        rec.type     = wal::log_record_type::COMMIT;
        rec.prev_lsn = it->last_lsn;

        auto lsn{TRY(manager.append_record(rec))};
        TRY(manager.flush(lsn));
    }

    active_txns_.erase(it);
    return {};
}

auto manager::abort_txn(id_t id, wal::log_manager& manager) -> result<void> {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    auto             it{TRY(find_id(id))};

    if (it->last_lsn) {
        wal::log_record rec;
        rec.txn_id   = id;
        rec.type     = wal::log_record_type::ABORT;
        rec.prev_lsn = it->last_lsn;

        auto lsn{TRY(manager.append_record(rec))};
        TRY(manager.flush(lsn));
    }

    active_txns_.erase(it);
    return {};
}

auto manager::update_txn_lsn(id_t id, wal::lsn_t lsn) -> result<void> {
    PROFILE_FUNCTION();
    std::unique_lock lock{mutex_};
    auto             it{TRY(find_id(id))};
    it->last_lsn = lsn;
    return {};
}

auto manager::snapshot_att() -> active_txn_buf_t {
    std::unique_lock lock{mutex_};
    return active_txns_;
}

} // namespace cairn::txn
