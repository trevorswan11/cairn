#pragma once

#include <condition_variable>
#include <mutex>

#include <stdx/fixed/hash_table.hh>
#include <stdx/fixed/vector.hh>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "txn/id.hh"

namespace cairn::txn::lock {

enum class mode_t : u8 {
    SHARED,
    EXCLUSIVE,
};

enum class resource_type_t : u8 {
    ROW,
    GAP,
};

using index_id_t = u64; // This is loosely typed to support different resources
using key_hash_t = u64; // This is loosely typed to support different resources

struct resource_id_t {
    index_id_t      index_id;
    key_hash_t      key_hash;
    resource_type_t type;

    [[nodiscard]] auto operator==(const resource_id_t&) const noexcept -> bool = default;
};

struct resource_id_hash {
    [[nodiscard]] static constexpr auto operator()(const resource_id_t& rid) noexcept -> u64 {
        stdx::hasher h;
        h.combine(rid.index_id);
        h.combine(rid.key_hash);
        h.combine(rid.type);
        return h.finalize();
    }
};

using txn_resources_t = stdx::fixed::vector<resource_id_t, 64>;

struct wait_state_t {
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done{false};
    bool                    aborted{false};
};

struct request_t {
    id_t                        txn_id;
    mode_t                      mode;
    bool                        granted{false};
    stdx::option<wait_state_t&> wait_state; // Points to stack allocated
};

struct bucket_t {
    stdx::fixed::vector<request_t, 16> requests;
};

// Global lock table mapping resource to its lock bucket
using bucket_table_t =
    stdx::fixed::auto_hash_map<resource_id_t, bucket_t, stdx::sizes::kib(1UZ), resource_id_hash>;

// Tracks resources locked by each transaction to enable automatic release
using tracked_txn_resources_t = stdx::fixed::auto_hash_map<id_t, txn_resources_t, 128, id_hash_t>;

// Set of currently wounded transactions
using wounded_txns_t = stdx::fixed::auto_hash_set<id_t, 128, id_hash_t>;

} // namespace cairn::txn::lock
