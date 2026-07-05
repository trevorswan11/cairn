#pragma once

#include <filesystem>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "txn/manager.hh"
#include "wal/checkpoints.hh"
#include "wal/log_manager.hh"
#include "wal/log_sequence_number.hh"

namespace cairn::wal {

template <usize PoolSize> class recovery_manager {
  public:
    recovery_manager(storage::buffer_pool<PoolSize>& pool,
                     txn::manager&                   tm,
                     log_manager&                    log_manager,
                     std::filesystem::path           control_path,
                     std::filesystem::path           log_path) noexcept
        : pool_{pool}, tm_{tm}, log_manager_{log_manager}, control_path_{std::move(control_path)},
          log_path_{std::move(log_path)} {}

    [[nodiscard]] auto recover() -> result<void> { TODO(); }

  private:
    using dirty_page_map_t = ankerl::unordered_dense::map<storage::page_id_t, lsn_t>;
    using active_txn_map_t = ankerl::unordered_dense::map<txn::id_t, checkpoint_att_entry>;

    struct analysis_result {
        active_txn_map_t active_txns;
        dirty_page_map_t dirty_pages;
    };

  private:
    [[nodiscard]] auto run_analysis() -> result<analysis_result> { TODO(); }

    [[nodiscard]] auto run_undo(const active_txn_map_t& active_txns) -> result<void> {
        TODO(active_txns);
    }

    [[nodiscard]] auto run_redo(const dirty_page_map_t& dirty_pages) -> result<void> {
        TODO(dirty_pages);
    }

  private:
    storage::buffer_pool<PoolSize>& pool_;
    txn::manager&                   tm_;
    log_manager&                    log_manager_;
    const std::filesystem::path     control_path_;
    const std::filesystem::path     log_path_;
};

} // namespace cairn::wal
