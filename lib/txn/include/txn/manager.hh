#pragma once

#include <algorithm>
#include <mutex>
#include <vector>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/error.hh"
#include "txn/id.hh"
#include "wal/checkpoints.hh"
#include "wal/manager.hh"
#include "wal/sequence_number.hh"

namespace cairn::txn {

class manager {
  public:
    using active_txn_buf_t = std::vector<wal::checkpoint_att_entry>;

  public:
    [[nodiscard]] auto begin_txn() -> id_t;
    [[nodiscard]] auto commit_txn(id_t id, wal::manager& manager) -> result<void>;
    [[nodiscard]] auto abort_txn(id_t id, wal::manager& manager) -> result<void>;
    [[nodiscard]] auto update_txn_lsn(id_t id, wal::lsn_t lsn) -> result<void>;
    [[nodiscard]] auto snapshot_att() -> active_txn_buf_t;

  private:
    [[nodiscard]] auto find_id(id_t id) noexcept -> result<active_txn_buf_t::iterator> {
        auto it{std::ranges::find_if(active_txns_,
                                     [id](const auto& entry) { return entry.txn_id == id; })};
        if (it == active_txns_.end()) { return stdx::err{error_t::TXN_NOT_FOUND}; }
        return it;
    }

  private:
    std::mutex       mutex_;
    active_txn_buf_t active_txns_;
    id_t             next_txn_id_{1};
};

} // namespace cairn::txn
