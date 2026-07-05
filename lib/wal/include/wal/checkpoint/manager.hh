#pragma once

#include <filesystem>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "support/error.hh"
#include "txn/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"

namespace cairn::wal::checkpoint {

class manager {
  public:
    explicit manager(std::filesystem::path control_path) noexcept;

    template <usize PoolSize>
    auto checkpoint(storage::buffer_pool<PoolSize>& pool,
                    txn::manager&                   tm,
                    log::manager&                   wal_manager) -> result<log::seq_num> {
        log::record begin_rec;
        begin_rec.type = log::record_type::CHECKPOINT_BEGIN;
        const auto begin_lsn{TRY(wal_manager.append_record(begin_rec))};

        auto dpt{pool.snapshot_dpt()};
        auto att{tm.snapshot_att()};

        log::record end_rec;
        end_rec.type = log::record_type::CHECKPOINT_END;
        end_rec.dpt  = dpt;
        end_rec.att  = att;

        const auto end_lsn{TRY(wal_manager.append_record(end_rec))};
        TRY(wal_manager.flush(end_lsn));
        TRY(persist_lsn(begin_lsn));
        return begin_lsn;
    }

    // Read the latest checkpoint LSN from the control block/file
    auto read_latest_checkpoint_lsn() -> result<log::seq_num>;

  private:
    auto persist_lsn(log::seq_num lsn) -> result<void>;

  private:
    const std::filesystem::path control_path_;
};

} // namespace cairn::wal::checkpoint
