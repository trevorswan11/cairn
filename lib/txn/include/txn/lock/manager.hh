#pragma once

#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/error.hh"
#include "txn/id.hh"
#include "txn/lock/types.hh"

namespace cairn::txn::lock {

class manager {
  public:
    template <mode_t Mode>
    [[nodiscard]] auto lock_row(id_t id, index_id_t index_id, u64 key_hash) {
        if constexpr (Mode == mode_t::SHARED) {
            return lock_row_shared(id, index_id, key_hash);
        } else {
            return lock_row_exclusive(id, index_id, key_hash);
        }
    }

    template <mode_t Mode>
    [[nodiscard]] auto lock_gap(id_t id, index_id_t index_id, u64 key_hash) {
        if constexpr (Mode == mode_t::SHARED) {
            return lock_gap_shared(id, index_id, key_hash);
        } else {
            return lock_gap_exclusive(id, index_id, key_hash);
        }
    }

    auto               release_all_locks(id_t id) -> void;
    [[nodiscard]] auto is_wounded(id_t id) const noexcept -> bool;

  private:
    [[nodiscard]] auto lock_row_shared(id_t id, index_id_t index_id, u64 key_hash)
        -> result<void>;
    [[nodiscard]] auto lock_row_exclusive(id_t id, index_id_t index_id, u64 key_hash)
        -> result<void>;
    [[nodiscard]] auto lock_gap_shared(id_t id, index_id_t index_id, u64 key_hash)
        -> result<void>;
    [[nodiscard]] auto lock_gap_exclusive(id_t id, index_id_t index_id, u64 key_hash)
        -> result<void>;

  private:
    bucket_table_t          bucket_table_;
    tracked_txn_resources_t tracked_txn_resources_;
    wounded_txns_t          wounded_txns_;
};

} // namespace cairn::txn::lock
