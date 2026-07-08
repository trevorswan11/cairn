#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "stdx/utility.hh"
#include "storage/bplus.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/read_set.hh"
#include "txn/snapshot.hh"

namespace cairn::exec {

template <typename IndexKey,
          typename PrimaryKey,
          usize MaxTupleSize,
          usize PoolSize,
          typename Compare        = std::less<IndexKey>,
          typename PrimaryCompare = std::less<PrimaryKey>>
class index_scan_read_set : public txn::read_set_t {
  public:
    using index_tree_t = storage::bplus_tree<IndexKey, PrimaryKey, PoolSize, Compare>;
    using txn_tree_t   = txn::iot_tree<PrimaryKey, MaxTupleSize, PoolSize, PrimaryCompare>;

  public:
    index_scan_read_set(index_tree_t& index, txn_tree_t& primary_tree)
        : index_{index}, primary_tree_{primary_tree} {}

    auto add_range(const IndexKey& low, const IndexKey& high) -> void {
        std::scoped_lock lock{mutex_};
        ranges_.emplace_back(low, high);
    }

    auto check_conflict(txn::id_t              reader_txn_id,
                        txn::timestamp_t       read_ts,
                        const get_commit_ts_t& get_commit_ts) const -> result<bool> override {
        std::scoped_lock lock{mutex_};

        for (const auto& range : ranges_) {
            bool conflict_detected{false};
            TRY(index_.range_scan(
                range.first, range.second, [&](const IndexKey&, const PrimaryKey& pk) -> bool {
                    if (auto get{primary_tree_.get_raw(pk)}) {
                        const auto header{get->first};
                        if (header.txn_id != txn::INVALID_TXN_ID &&
                            header.txn_id != reader_txn_id) {
                            // Stop the scan once a conflict is detected
                            auto commit_ts_opt{get_commit_ts(header.txn_id)};
                            if (commit_ts_opt && *commit_ts_opt > read_ts) {
                                conflict_detected = true;
                                return false;
                            }
                        }
                    }
                    return true;
                }));
            if (conflict_detected) { return true; }
        }
        return false;
    }

  private:
    mutable std::mutex                         mutex_;
    index_tree_t&                              index_;
    txn_tree_t&                                primary_tree_;
    std::vector<std::pair<IndexKey, IndexKey>> ranges_;
};

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
    using index_scan_read_set_t =
        index_scan_read_set<IndexKey, PrimaryKey, MaxTupleSize, PoolSize, Compare, PrimaryCompare>;

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

        if (auto rs{txn_mgr_.get_or_create_read_set(
                reader_txn_id_, index_.tree_id(), [&] -> stdx::box<txn::read_set_t> {
                    return stdx::make_box<index_scan_read_set_t>(index_, primary_tree_);
                })}) {
            static_cast<index_scan_read_set_t&>(*rs).add_range(low, high);
        }

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
