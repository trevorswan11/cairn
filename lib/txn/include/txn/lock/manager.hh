#pragma once

#include <mutex>

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
    [[nodiscard]] auto lock_row_shared(id_t id, index_id_t index_id, u64 key_hash) {
        return acquire_lock(id, {index_id, key_hash, resource_type_t::ROW}, mode_t::SHARED);
    }

    [[nodiscard]] auto lock_row_exclusive(id_t id, index_id_t index_id, u64 key_hash) {
        return acquire_lock(id, {index_id, key_hash, resource_type_t::ROW}, mode_t::EXCLUSIVE);
    }

    [[nodiscard]] auto lock_gap_shared(id_t id, index_id_t index_id, u64 key_hash) {
        return acquire_lock(id, {index_id, key_hash, resource_type_t::GAP}, mode_t::SHARED);
    }

    [[nodiscard]] auto lock_gap_exclusive(id_t id, index_id_t index_id, u64 key_hash) {
        return acquire_lock(id, {index_id, key_hash, resource_type_t::GAP}, mode_t::EXCLUSIVE);
    }

    [[nodiscard]] auto is_wounded(id_t id) const noexcept -> bool;

    auto release_all_locks(id_t id) -> void;
    auto release_shared_locks(id_t id) -> void;

  private:
    // Returns true if the transaction no longer holds any locks on this resource, false otherwise.
    [[nodiscard]] auto release_locks_in_bucket(id_t                 id,
                                               resource_id_t        res_id,
                                               stdx::option<mode_t> filter_mode) -> bool;

    [[nodiscard]] auto acquire_lock(id_t id, resource_id_t res_id, mode_t mode) -> result<void>;
    auto               wound(id_t id) -> void;
    [[nodiscard]] auto add_tracked_resource(id_t id, resource_id_t res_id) -> result<void>;
    auto               remove_tracked_resource(id_t id, resource_id_t res_id) -> void;

    [[nodiscard]] static constexpr auto conflicts(mode_t a, mode_t b) noexcept -> bool {
        return a == mode_t::EXCLUSIVE || b == mode_t::EXCLUSIVE;
    }

  private:
    mutable std::mutex      mutex_;
    bucket_table_t          bucket_table_;
    tracked_txn_resources_t tracked_txn_resources_;
    wounded_txns_t          wounded_txns_;
};

} // namespace cairn::txn::lock
