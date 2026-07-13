#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <stdx/utility.hh>
#include <type_traits>
#include <vector>

#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/diagnostic/error.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/snapshot.hh"

namespace cairn::exec {

template <typename Key, usize MaxTupleSize, usize PoolSize, typename Compare = std::less<Key>>
class table_scan {
  public:
    using txn_tree_t          = txn::iot_tree<Key, MaxTupleSize, PoolSize, Compare>;
    using iot_tree_read_set_t = txn::iot_tree_read_set<Key, MaxTupleSize, PoolSize, Compare>;

    table_scan(txn_tree_t&            tree,
               txn::id_t              reader_txn_id,
               const txn::snapshot_t& snap,
               const txn::manager&    txn_mgr) noexcept
        : tree_{tree}, reader_txn_id_{reader_txn_id}, snap_{snap}, txn_mgr_{txn_mgr} {}

    template <typename Fn>
    auto operator()(const Key& low, const Key& high, Fn&& visitor, bool inclusive = true)
        -> result<usize> {
        using fn_result_t = std::invoke_result_t<Fn, const Key&, gsl::span<const std::byte>>;
        std::vector<std::byte> payload_buf;

        const auto level{TRY(txn_mgr_.get_isolation_level(reader_txn_id_))};
        if (auto rs{txn_mgr_.get_or_create_read_set(reader_txn_id_, tree_.tree_id(), [&] {
                return stdx::make_box<iot_tree_read_set_t>(tree_);
            })}) {
            static_cast<iot_tree_read_set_t&>(*rs).add_range(low, high);
        }

        auto active_snap{snap_};
        if (level == txn::isolation_level_t::READ_COMMITTED) {
            active_snap = TRY(txn_mgr_.acquire_snapshot(reader_txn_id_));
        }
        const txn::shared_lock_guard_t guard{
            txn_mgr_, reader_txn_id_, level == txn::isolation_level_t::READ_COMMITTED};

        usize        count{0};
        result<void> scan_res{};
        TRY(tree_.tree().range_scan(
            low,
            high,
            [&](const Key& k, gsl::span<const std::byte> val) -> bool {
                if (level == txn::isolation_level_t::READ_COMMITTED ||
                    level == txn::isolation_level_t::REPEATABLE_READ) {
                    if (auto lock_res{txn_mgr_.lock_row_shared(
                            reader_txn_id_, tree_.tree_id(), stdx::hasher{}.combine(k).finalize())};
                        !lock_res) {
                        scan_res = lock_res;
                        return false;
                    }
                }

                const auto header{txn::read_version_header(val)};
                const auto payload{txn::read_payload(val)};

                auto resolved{tree_.resolve_version(
                    reader_txn_id_, active_snap, txn_mgr_, header, payload, payload_buf)};
                if (!resolved) {
                    scan_res = resolved.error();
                    return false;
                }

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
        TRY(scan_res);

        return count;
    }

  private:
    txn_tree_t&            tree_;
    txn::id_t              reader_txn_id_;
    const txn::snapshot_t& snap_;
    const txn::manager&    txn_mgr_;
};

} // namespace cairn::exec
