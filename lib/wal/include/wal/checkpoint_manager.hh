#pragma once

#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <ios>
#include <system_error>
#include <utility>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "support/error.hh"
#include "txn/manager.hh"
#include "wal/log_manager.hh"
#include "wal/log_record.hh"
#include "wal/log_sequence_number.hh"

namespace cairn::wal {

template <usize PoolSize> class checkpoint_manager {
  public:
    explicit checkpoint_manager(std::filesystem::path control_path) noexcept
        : control_path_{std::move(control_path)} {}

    auto checkpoint(storage::buffer_pool<PoolSize>& pool,
                    txn::manager&                   tm,
                    log_manager&                    wal_manager) -> result<lsn_t> {
        log_record begin_rec;
        begin_rec.type = log_record_type::CHECKPOINT_BEGIN;
        const auto begin_lsn{TRY(wal_manager.append_record(begin_rec))};

        auto dpt{pool.snapshot_dpt()};
        auto att{tm.snapshot_att()};

        log_record end_rec;
        end_rec.type = log_record_type::CHECKPOINT_END;
        end_rec.dpt  = dpt;
        end_rec.att  = att;

        const auto end_lsn{TRY(wal_manager.append_record(end_rec))};
        TRY(wal_manager.flush(end_lsn));
        TRY(persist_lsn(begin_lsn));
        return begin_lsn;
    }

    // Read the latest checkpoint LSN from the control block/file
    auto read_latest_checkpoint_lsn() -> result<lsn_t> {
        if (!std::filesystem::exists(control_path_)) {
            return stdx::err{error_t::WAL_CONTROL_PATH_NOT_FOUND};
        }

        std::ifstream in{control_path_, std::ios::in | std::ios::binary};
        if (!in.is_open()) { return stdx::err{error_t::IO_ERROR}; }
        std::array<char, sizeof(lsn_t)> checkpoint_lsn;
        in.read(checkpoint_lsn.data(), checkpoint_lsn.size());
        if (in.fail()) { return stdx::err{error_t::IO_ERROR}; }
        return std::bit_cast<lsn_t>(checkpoint_lsn);
    }

  private:
    auto persist_lsn(lsn_t lsn) -> result<void> {
        auto temp_path{control_path_};
        temp_path.replace_extension(".tmp");

        {
            std::ofstream out{temp_path, std::ios::out | std::ios::binary | std::ios::trunc};
            if (!out.is_open()) { return stdx::err{error_t::IO_ERROR}; }
            out.write(reinterpret_cast<const char*>(&lsn), sizeof(lsn));
        }

        std::error_code ec;
        std::filesystem::rename(temp_path, control_path_, ec);
        if (ec) { return stdx::err{error_t::IO_ERROR}; }
        return {};
    }

  private:
    const std::filesystem::path control_path_;
};

} // namespace cairn::wal
