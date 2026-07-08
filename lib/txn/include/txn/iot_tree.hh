#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/fixed/vector.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/bplus.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "txn/manager.hh"
#include "txn/read_set.hh"
#include "txn/snapshot.hh"
#include "txn/undo/manager.hh"

namespace cairn::txn {

struct tuple_version_header_t {
    stdx::option<txn::id_t>   txn_id;
    bool                      is_timestamp;
    bool                      is_deleted;
    stdx::option<undo::ptr_t> undo_ptr;
};

auto read_version_header(gsl::span<const std::byte> val) -> tuple_version_header_t;
auto read_payload(gsl::span<const std::byte> val) -> gsl::span<const std::byte>;

template <typename Key, usize MaxTupleSize, usize PoolSize, typename Compare> class iot_tree;

template <typename Key, usize MaxTupleSize, usize PoolSize, typename Compare = std::less<Key>>
class iot_tree_read_set : public read_set_t {
  public:
    using tree_t = iot_tree<Key, MaxTupleSize, PoolSize, Compare>;

  public:
    iot_tree_read_set(const tree_t& tree) : tree_{tree} {}

    auto add_key(const Key& key) -> void {
        std::scoped_lock lock{mutex_};
        keys_.emplace_back(key);
    }

    auto add_range(const Key& low, const Key& high) -> void {
        std::scoped_lock lock{mutex_};
        ranges_.emplace_back(low, high);
    }

    auto check_conflict(id_t                   reader_txn_id,
                        timestamp_t            read_ts,
                        const get_commit_ts_t& get_commit_ts) const -> result<bool> override {
        std::scoped_lock lock{mutex_};

        for (const auto& key : keys_) {
            if (auto get_res{tree_.get_raw(key)}) {
                const auto header{get_res->first};
                if (header.txn_id && *header.txn_id != reader_txn_id) {
                    auto commit_ts_opt{get_commit_ts(*header.txn_id)};
                    if (commit_ts_opt && *commit_ts_opt > read_ts) { return true; }
                }
            }
        }

        for (const auto& range : ranges_) {
            bool conflict_detected = false;
            TRY(tree_.tree().range_scan(
                range.first, range.second, [&](const Key&, gsl::span<const std::byte> val) -> bool {
                    const auto header = read_version_header(val);
                    if (header.txn_id && *header.txn_id != reader_txn_id) {
                        auto commit_ts_opt{get_commit_ts(*header.txn_id)};
                        if (commit_ts_opt && *commit_ts_opt > read_ts) {
                            conflict_detected = true;
                            return false;
                        }
                    }
                    return true;
                }));
            if (conflict_detected) { return true; }
        }

        return false;
    }

  private:
    mutable std::mutex               mutex_;
    const tree_t&                    tree_;
    std::vector<Key>                 keys_;
    std::vector<std::pair<Key, Key>> ranges_;
};

template <typename Key, usize MaxTupleSize, usize PoolSize, typename Compare = std::less<Key>>
class iot_tree {
  public:
    using tree_t =
        storage::iot_tree<Key, MaxTupleSize + sizeof(tuple_version_header_t), PoolSize, Compare>;
    using buf_t               = stdx::fixed::vector<std::byte, tree_t::pool_size>;
    using iot_tree_read_set_t = iot_tree_read_set<Key, MaxTupleSize, PoolSize, Compare>;

  public:
    iot_tree(tree_t& tree, undo::manager<Key, PoolSize>& undo_mgr) noexcept
        : tree_{tree}, undo_mgr_{undo_mgr} {}

    [[nodiscard]] auto insert_txn(txn::id_t id, const Key& key, gsl::span<const std::byte> payload)
        -> result<void> {
        if (const auto get{tree_.get(key)}) {
            if (!read_version_header(*get).is_deleted) {
                return stdx::err{error_t::STORAGE_DUPLICATE_KEY};
            }
            return update_txn(id, key, payload);
        } else if (get.error() != error_t::STORAGE_KEY_NOT_FOUND) {
            return stdx::err{get.error()};
        }

        const tuple_version_header_t header{
            .txn_id       = id,
            .is_timestamp = false,
            .is_deleted   = false,
            .undo_ptr     = stdx::none,
        };

        const auto buf{make_buf(header, payload)};
        TRY(tree_.emplace(key, gsl::span<const std::byte>{buf}));
        TRY(undo_mgr_.append_record(
            id, key, undo::op_t::INSERT, false, false, stdx::none, stdx::none, {}));

        return {};
    }

    [[nodiscard]] auto update_txn(txn::id_t id, const Key& key, gsl::span<const std::byte> payload)
        -> result<void> {
        const auto get{TRY(tree_.get(key))};
        const auto old_val{snapshot(get)};
        const auto old_header{read_version_header(old_val)};
        const auto old_payload{read_payload(old_val)};

        const auto undo_ptr{TRY(undo_mgr_.append_record(id,
                                                        key,
                                                        undo::op_t::UPDATE,
                                                        old_header.is_timestamp,
                                                        old_header.is_deleted,
                                                        old_header.txn_id,
                                                        old_header.undo_ptr,
                                                        old_payload))};

        const tuple_version_header_t new_header{
            .txn_id       = id,
            .is_timestamp = false,
            .is_deleted   = false,
            .undo_ptr     = undo_ptr,
        };

        const auto buf{make_buf(new_header, payload)};
        TRY(tree_.update(key, gsl::span<const std::byte>{buf}));
        return {};
    }

    [[nodiscard]] auto delete_txn(txn::id_t id, const Key& key) -> result<void> {
        const auto get{TRY(tree_.get(key))};
        const auto old_val{snapshot(get)};
        const auto old_header{read_version_header(old_val)};
        if (old_header.is_deleted) { return stdx::err{error_t::STORAGE_KEY_NOT_FOUND}; }
        const auto old_payload{read_payload(old_val)};

        const auto undo_ptr{TRY(undo_mgr_.append_record(id,
                                                        key,
                                                        undo::op_t::DELETE,
                                                        old_header.is_timestamp,
                                                        old_header.is_deleted,
                                                        old_header.txn_id,
                                                        old_header.undo_ptr,
                                                        old_payload))};

        const tuple_version_header_t new_header{
            .txn_id       = id,
            .is_timestamp = false,
            .is_deleted   = true,
            .undo_ptr     = undo_ptr,
        };

        stdx::fixed::vector<std::byte, sizeof(tuple_version_header_t)> buf;
        buf.resize(buf.capacity());
        std::memcpy(buf.data(), &new_header, buf.capacity());
        return tree_.update(key, buf);
    }

    [[nodiscard]] auto rollback_txn(txn::id_t id) -> result<void> {
        auto                   cur_ptr{undo_mgr_.get_last_txn_undo(id)};
        std::vector<std::byte> payload;

        while (cur_ptr) {
            const auto rec{TRY(undo_mgr_.read_record(*cur_ptr, payload))};

            switch (rec.op) {
            case undo::op_t::INSERT: {
                TRY(tree_.remove(rec.key));
                break;
            }
            case undo::op_t::UPDATE:
            case undo::op_t::DELETE: {
                const tuple_version_header_t old_header{
                    .txn_id       = rec.txn_id,
                    .is_timestamp = rec.is_timestamp,
                    .is_deleted   = rec.is_deleted,
                    .undo_ptr     = rec.prev_undo_ptr,
                };

                auto buf{make_buf(old_header, payload)};
                TRY(tree_.update(rec.key, buf));
                break;
            }
            }

            cur_ptr = rec.prev_txn_undo_ptr;
        }

        undo_mgr_.remove_txn(id);
        return {};
    }

    [[nodiscard]] auto get_raw(const Key& key) const
        -> result<std::pair<tuple_version_header_t, gsl::span<const std::byte>>> {
        const auto val{TRY(tree_.get(key))};
        const auto header{read_version_header(val)};
        const auto payload{read_payload(val)};
        return std::make_pair(header, payload);
    }

    [[nodiscard]] auto tree(this auto&& self) noexcept -> auto& { return self.tree_; }
    [[nodiscard]] auto undo_manager(this auto&& self) noexcept -> auto& { return self.undo_mgr_; }
    [[nodiscard]] auto tree_id() const noexcept -> storage::tree_id_t { return tree_.tree_id(); }

    [[nodiscard]] auto resolve_version(txn::id_t                     reader_txn_id,
                                       const snapshot_t&             snap,
                                       const manager&                txn_mgr,
                                       const tuple_version_header_t& latest_header,
                                       gsl::span<const std::byte>    latest_payload,
                                       std::vector<std::byte>&       payload_buf) const
        -> result<stdx::option<gsl::span<const std::byte>>> {
        if (txn_mgr.is_visible(
                snap, reader_txn_id, latest_header.txn_id, latest_header.is_timestamp)) {
            if (latest_header.is_deleted) { return stdx::none; }
            payload_buf.resize(latest_payload.size_bytes());
            if (!latest_payload.empty()) {
                std::memcpy(payload_buf.data(), latest_payload.data(), latest_payload.size_bytes());
            }
            return gsl::span{payload_buf.data(), payload_buf.size()};
        }

        stdx::option<undo::ptr_t> cur_ptr{latest_header.undo_ptr};
        while (cur_ptr) {
            const auto rec{TRY(undo_mgr_.read_record(*cur_ptr, payload_buf))};
            if (txn_mgr.is_visible(snap, reader_txn_id, rec.txn_id, rec.is_timestamp)) {
                if (rec.is_deleted) { return stdx::none; }
                return gsl::span{payload_buf.data(), payload_buf.size()};
            }
            cur_ptr = rec.prev_undo_ptr;
        }

        return stdx::none;
    }

    [[nodiscard]] auto get_txn(txn::id_t               reader_txn_id,
                               const snapshot_t&       snap,
                               const manager&          txn_mgr,
                               const Key&              key,
                               std::vector<std::byte>& payload_buf) const
        -> result<stdx::option<gsl::span<const std::byte>>> {
        if (auto rs{txn_mgr.get_or_create_read_set(reader_txn_id, tree_id(), [&] {
                return stdx::make_box<iot_tree_read_set_t>(*this);
            })}) {
            static_cast<iot_tree_read_set_t&>(*rs).add_key(key);
        }

        const auto get_res{tree_.get(key)};
        if (!get_res) {
            if (get_res.error() == error_t::STORAGE_KEY_NOT_FOUND) { return stdx::none; }
            return stdx::err{get_res.error()};
        }

        const auto val{*get_res};
        const auto header{read_version_header(val)};
        const auto payload{read_payload(val)};
        return resolve_version(reader_txn_id, snap, txn_mgr, header, payload, payload_buf);
    }

  private:
    [[nodiscard]] static auto make_buf(const tuple_version_header_t& header,
                                       gsl::span<const std::byte>    payload) noexcept -> buf_t {
        buf_t buf;
        buf.resize(sizeof(tuple_version_header_t) + payload.size_bytes());
        std::memcpy(buf.data(), &header, sizeof(tuple_version_header_t));
        if (!payload.empty()) {
            std::memcpy(
                buf.data() + sizeof(tuple_version_header_t), payload.data(), payload.size_bytes());
        }
        return buf;
    }

    // Snapshot a tree-owned span into a fixed buffer
    [[nodiscard]] static auto snapshot(gsl::span<const std::byte> span) noexcept -> buf_t {
        buf_t buf;
        buf.resize(span.size_bytes());
        if (!span.empty()) { std::memcpy(buf.data(), span.data(), buf.size()); }
        return buf;
    }

  private:
    tree_t&                       tree_;
    undo::manager<Key, PoolSize>& undo_mgr_;
};

} // namespace cairn::txn
