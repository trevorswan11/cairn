#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <type_traits>
#include <vector>

#include <gsl/span>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/error.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/snapshot.hh"

namespace cairn::exec {

template <typename Key, usize MaxTupleSize, usize PoolSize, typename Compare = std::less<Key>>
class table_scan {
  public:
    using txn_tree_t = txn::iot_tree<Key, MaxTupleSize, PoolSize, Compare>;

    table_scan(txn_tree_t&            tree,
               txn::id_t              reader_txn_id,
               const txn::snapshot_t& snap,
               const txn::manager&    txn_mgr) noexcept
        : tree_{tree}, reader_txn_id_{reader_txn_id}, snap_{snap}, txn_mgr_{txn_mgr} {}

    template <typename Fn>
    auto scan(const Key& low, const Key& high, Fn&& visitor, bool inclusive = true)
        -> result<usize> {
        using fn_result_t = std::invoke_result_t<Fn, const Key&, gsl::span<const std::byte>>;
        std::vector<std::byte> payload_buf;

        usize count{0};
        TRY(tree_.tree().range_scan(
            low,
            high,
            [&](const Key& k, gsl::span<const std::byte> val) -> bool {
                const auto header{txn::read_version_header(val)};
                const auto payload{txn::read_payload(val)};

                auto resolved{tree_.resolve_version(
                    reader_txn_id_, snap_, txn_mgr_, header, payload, payload_buf)};
                if (!resolved) { return false; } // Stop scan on error

                if (resolved.value()) {
                    count++;
                    if constexpr (std::same_as<fn_result_t, bool>) {
                        return visitor(k, *resolved.value());
                    } else {
                        visitor(k, *resolved.value());
                        return true;
                    }
                }
                return true;
            },
            inclusive));

        return count;
    }

  private:
    txn_tree_t&            tree_;
    txn::id_t              reader_txn_id_;
    const txn::snapshot_t& snap_;
    const txn::manager&    txn_mgr_;
};

} // namespace cairn::exec
