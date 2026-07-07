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

#include "storage/bplus.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/snapshot.hh"

namespace cairn::exec {

template <typename IndexKey,
          typename PrimaryKey,
          usize MaxTupleSize,
          usize PoolSize,
          typename Compare        = std::less<IndexKey>,
          typename PrimaryCompare = std::less<PrimaryKey>>
class index_scan {
  public:
    using index_tree_t = storage::bplus_tree<IndexKey, PrimaryKey, PoolSize, Compare>;
    using txn_tree_t   = txn::iot_tree<PrimaryKey, MaxTupleSize, PoolSize, PrimaryCompare>;

    index_scan(index_tree_t&          index,
               txn_tree_t&            primary_tree,
               txn::id_t              reader_txn_id,
               const txn::snapshot_t& snap,
               const txn::manager&    txn_mgr) noexcept
        : index_{index}, primary_tree_{primary_tree}, reader_txn_id_{reader_txn_id}, snap_{snap},
          txn_mgr_{txn_mgr} {}

    template <typename Fn>
    auto operator()(const IndexKey& low, const IndexKey& high, Fn&& visitor, bool inclusive = true)
        -> result<usize> {
        using fn_result_t = std::invoke_result_t<Fn, const PrimaryKey&, gsl::span<const std::byte>>;
        usize count{0};

        std::vector<std::byte> payload_buf;
        TRY(index_.range_scan(
            low,
            high,
            [&](const IndexKey&, const PrimaryKey& pk) -> bool {
                auto get_res{primary_tree_.get_raw(pk)};
                if (!get_res) {
                    if (get_res.error() == error_t::STORAGE_KEY_NOT_FOUND) { return true; }
                    return false;
                }

                const auto [header, payload]{*get_res};
                auto resolved{primary_tree_.resolve_version(
                    reader_txn_id_, snap_, txn_mgr_, header, payload, payload_buf)};
                if (!resolved) { return false; }

                if (resolved.value()) {
                    count++;

                    if constexpr (std::same_as<fn_result_t, bool>) {
                        return visitor(pk, *resolved.value());
                    } else {
                        visitor(pk, *resolved.value());
                        return true;
                    }
                }
                return true;
            },
            inclusive));

        return count;
    }

  private:
    index_tree_t&          index_;
    txn_tree_t&            primary_tree_;
    txn::id_t              reader_txn_id_;
    const txn::snapshot_t& snap_;
    const txn::manager&    txn_mgr_;
};

} // namespace cairn::exec
