#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/fixed/vector.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/bplus.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "txn/undo.hh"

namespace cairn::txn {

struct tuple_version_header_t {
    txn::id_t                txn_id;
    bool                     is_timestamp;
    bool                     is_deleted;
    stdx::option<undo_ptr_t> undo_ptr;
};

inline auto read_version_header(gsl::span<const std::byte> val) -> tuple_version_header_t {
    tuple_version_header_t header;
    std::memcpy(&header, val.data(), sizeof(tuple_version_header_t));
    return header;
}

inline auto read_payload(gsl::span<const std::byte> val) -> gsl::span<const std::byte> {
    return val.subspan(sizeof(tuple_version_header_t));
}

template <typename Key, usize MaxTupleSize, usize PoolSize = 64, typename Compare = std::less<Key>>
class iot_tree {
  public:
    using tree_t =
        storage::iot_tree<Key, MaxTupleSize + sizeof(tuple_version_header_t), PoolSize, Compare>;

  public:
    iot_tree(tree_t& tree, undo_manager<Key, PoolSize>& undo_mgr) noexcept
        : tree_{tree}, undo_mgr_{undo_mgr} {}

    [[nodiscard]] auto insert_txn(txn::id_t                  txn_id,
                                  const Key&                 key,
                                  gsl::span<const std::byte> payload) -> result<void> {
        if (auto get{tree_.get(key)}) {
            std::vector<std::byte>     old_val_buf((*get).begin(), (*get).end());
            gsl::span<const std::byte> old_val{old_val_buf};
            auto                       header = read_version_header(old_val);
            if (!header.is_deleted) { return stdx::err{error_t::STORAGE_DUPLICATE_KEY}; }
            return update_txn(txn_id, key, payload);
        } else if (get.error() != error_t::STORAGE_KEY_NOT_FOUND) {
            return stdx::err{get.error()};
        }

        tuple_version_header_t header{
            .txn_id       = txn_id,
            .is_timestamp = false,
            .is_deleted   = false,
            .undo_ptr     = stdx::none,
        };

        std::vector<std::byte> buf(sizeof(tuple_version_header_t) + payload.size_bytes());
        std::memcpy(buf.data(), &header, sizeof(tuple_version_header_t));
        if (!payload.empty()) {
            std::memcpy(
                buf.data() + sizeof(tuple_version_header_t), payload.data(), payload.size_bytes());
        }

        TRY(tree_.emplace(key, gsl::span<const std::byte>{buf}));
        TRY(undo_mgr_.append_record(
            txn_id, key, undo_op_t::INSERT, false, false, txn::INVALID_TXN_ID, stdx::none, {}));

        return {};
    }

    [[nodiscard]] auto update_txn(txn::id_t                  txn_id,
                                  const Key&                 key,
                                  gsl::span<const std::byte> payload) -> result<void> {
        auto                       get{TRY(tree_.get(key))};
        std::vector<std::byte>     old_val_buf(get.begin(), get.end());
        gsl::span<const std::byte> old_val{old_val_buf};
        auto                       old_header  = read_version_header(old_val);
        auto                       old_payload = read_payload(old_val);

        auto undo_ptr{TRY(undo_mgr_.append_record(txn_id,
                                                  key,
                                                  undo_op_t::UPDATE,
                                                  old_header.is_timestamp,
                                                  old_header.is_deleted,
                                                  old_header.txn_id,
                                                  old_header.undo_ptr,
                                                  old_payload))};

        tuple_version_header_t new_header{
            .txn_id       = txn_id,
            .is_timestamp = false,
            .is_deleted   = false,
            .undo_ptr     = undo_ptr,
        };

        std::vector<std::byte> buf(sizeof(tuple_version_header_t) + payload.size_bytes());
        std::memcpy(buf.data(), &new_header, sizeof(tuple_version_header_t));
        if (!payload.empty()) {
            std::memcpy(
                buf.data() + sizeof(tuple_version_header_t), payload.data(), payload.size_bytes());
        }

        TRY(tree_.update(key, gsl::span<const std::byte>{buf}));
        return {};
    }

    [[nodiscard]] auto delete_txn(txn::id_t txn_id, const Key& key) -> result<void> {
        auto get_res{TRY(tree_.get(key))};

        std::vector<std::byte>     old_val_buf(get_res.begin(), get_res.end());
        gsl::span<const std::byte> old_val{old_val_buf};
        auto                       old_header{read_version_header(old_val)};
        if (old_header.is_deleted) { return stdx::err{error_t::STORAGE_KEY_NOT_FOUND}; }
        auto old_payload{read_payload(old_val)};

        auto undo_ptr{TRY(undo_mgr_.append_record(txn_id,
                                                  key,
                                                  undo_op_t::DELETE,
                                                  old_header.is_timestamp,
                                                  old_header.is_deleted,
                                                  old_header.txn_id,
                                                  old_header.undo_ptr,
                                                  old_payload))};

        tuple_version_header_t new_header{
            .txn_id       = txn_id,
            .is_timestamp = false,
            .is_deleted   = true,
            .undo_ptr     = undo_ptr,
        };

        stdx::fixed::vector<std::byte, sizeof(tuple_version_header_t)> buf;
        buf.resize(buf.capacity());
        std::memcpy(buf.data(), &new_header, buf.capacity());
        TRY(tree_.update(key, gsl::span<const std::byte>{buf}));
        return {};
    }

    [[nodiscard]] auto rollback_txn(txn::id_t txn_id) -> result<void> {
        auto cur_ptr{undo_mgr_.get_last_txn_undo(txn_id)};

        while (cur_ptr) {
            auto        read_res{TRY(undo_mgr_.read_record(*cur_ptr))};
            const auto& rec{read_res.record};
            const auto& payload{read_res.payload};

            switch (rec.op) {
            case undo_op_t::INSERT: {
                TRY(tree_.remove(rec.key));
                break;
            }
            case undo_op_t::UPDATE:
            case undo_op_t::DELETE: {
                tuple_version_header_t old_header{
                    .txn_id       = rec.txn_id,
                    .is_timestamp = rec.is_timestamp,
                    .is_deleted   = rec.is_deleted,
                    .undo_ptr     = rec.prev_undo_ptr,
                };

                std::vector<std::byte> buf(sizeof(tuple_version_header_t) + payload.size());
                std::memcpy(buf.data(), &old_header, sizeof(tuple_version_header_t));
                if (!payload.empty()) {
                    std::memcpy(buf.data() + sizeof(tuple_version_header_t),
                                payload.data(),
                                payload.size());
                }

                TRY(tree_.update(rec.key, gsl::span<const std::byte>{buf}));
                break;
            }
            }

            cur_ptr = rec.prev_txn_undo_ptr;
        }

        undo_mgr_.remove_txn(txn_id);
        return {};
    }

    [[nodiscard]] auto get_raw(const Key& key)
        -> result<std::pair<tuple_version_header_t, gsl::span<const std::byte>>> {
        auto val     = TRY(tree_.get(key));
        auto header  = read_version_header(val);
        auto payload = read_payload(val);
        return std::make_pair(header, payload);
    }

  private:
    tree_t&                      tree_;
    undo_manager<Key, PoolSize>& undo_mgr_;
};

} // namespace cairn::txn
