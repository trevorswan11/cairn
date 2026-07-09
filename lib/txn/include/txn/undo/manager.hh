#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "txn/manager.hh"

namespace cairn::txn::undo {

struct ptr_t {
    storage::page_id_t page_id;
    u16                offset;

    constexpr auto operator==(const ptr_t&) const noexcept -> bool = default;
};

} // namespace cairn::txn::undo

namespace stdx {

template <> struct nullable<cairn::txn::undo::ptr_t> {
    using undo_ptr_t = cairn::txn::undo::ptr_t;
    [[nodiscard]] static constexpr auto invalid() noexcept -> undo_ptr_t {
        return {
            .page_id = cairn::storage::INVALID_PAGE_ID,
            .offset  = 0,
        };
    }

    [[nodiscard]] static constexpr auto is_valid(const undo_ptr_t& id) noexcept -> bool {
        return id != invalid();
    }
};

} // namespace stdx

namespace cairn::txn::undo {

enum class op_t : u8 {
    INSERT,
    UPDATE,
    DELETE,
};

template <typename Key> struct record_t {
    stdx::option<txn::id_t> txn_id;
    bool                    is_timestamp{false};
    bool                    is_deleted{false};
    stdx::option<ptr_t>     prev_undo_ptr;
    stdx::option<ptr_t>     prev_txn_undo_ptr;
    Key                     key;
    op_t                    op{op_t::INSERT};
    u32                     payload_size{0};
};

struct page_header_t {
    u16 free_space_ptr{sizeof(page_header_t)};
};

template <typename Key, usize PoolSize> class manager {
  public:
    using active_txn_undo_map_t = ankerl::unordered_dense::map<txn::id_t, ptr_t, txn::id_hash_t>;
    using page_active_records_map_t =
        ankerl::unordered_dense::map<storage::page_id_t, u32, storage::page_id_hash_t>;

  public:
    explicit manager(storage::buffer_pool<PoolSize>& pool) noexcept : pool_{pool} {}
    ~manager() = default;

    auto set_txn_manager(stdx::option<const txn::manager&> txn_mgr) noexcept -> void {
        txn_mgr_ = txn_mgr;
    }

    // Appends an undo record sequentially to the active undo page, allocating if needed
    [[nodiscard]] auto append_record(txn::id_t                  txn_id,
                                     const Key&                 key,
                                     op_t                       op,
                                     bool                       is_timestamp,
                                     bool                       is_deleted,
                                     stdx::option<txn::id_t>    version_txn_id,
                                     stdx::option<ptr_t>        prev_undo_ptr,
                                     gsl::span<const std::byte> payload) -> result<ptr_t> {
        bool                           reclaim_old_chain{false};
        stdx::option<txn::timestamp_t> horizon_opt;
        if (txn_mgr_) {
            const auto horizon{txn_mgr_->snapshot_horizon()};
            horizon_opt.emplace(horizon);
            if (version_txn_id) {
                if (txn_mgr_->committed_before_horizon(*version_txn_id, horizon)) {
                    reclaim_old_chain = true;
                }
            }
        }

        std::lock_guard lock{mutex_};

        // Perform transaction-level garbage collection
        if (txn_mgr_ && horizon_opt) {
            const auto             horizon{*horizon_opt};
            std::vector<txn::id_t> txns_to_reclaim;
            for (const auto& [tid, _] : active_txn_undo_) {
                if (txn_mgr_->committed_before_horizon(tid, horizon)) {
                    txns_to_reclaim.emplace_back(tid);
                }
            }

            for (auto tid : txns_to_reclaim) {
                if (auto it{active_txn_undo_.find(tid)}; it != active_txn_undo_.end()) {
                    reclaim_undo_chain_locked(it->second, &record_t<Key>::prev_txn_undo_ptr);
                    active_txn_undo_.erase(it);
                }
            }
        }

        stdx::option<ptr_t> actual_prev{prev_undo_ptr};
        if (reclaim_old_chain) { actual_prev = stdx::none; }

        const usize rec_size{sizeof(record_t<Key>) + payload.size_bytes()};
        if (rec_size > storage::DB_PAGE_SIZE - sizeof(page_header_t)) {
            return stdx::err{error_t::STORAGE_TREE_CORRUPT};
        }

        bool need_new{false};
        if (!active_page_id_) {
            need_new = true;
        } else {
            auto guard{TRY(pool_.fetch_write(*active_page_id_))};
            auto header{guard.template as<page_header_t>()};
            if (header->free_space_ptr + rec_size > storage::DB_PAGE_SIZE) { need_new = true; }
        }

        if (need_new) {
            auto [pid, guard]{TRY(pool_.new_write())};
            auto header{guard.template as<page_header_t>()};
            header->free_space_ptr = sizeof(page_header_t);
            guard.mark_dirty();
            active_page_id_.emplace(pid);
        }

        // Fetch active page and append
        const auto pid{*active_page_id_};
        page_active_records_[pid]++;
        auto      guard{TRY(pool_.fetch_write(pid))};
        auto      header{guard.template as<page_header_t>()};
        const u16 offset{header->free_space_ptr};

        stdx::option<ptr_t> prev_txn_undo;
        auto [txn_id_it, inserted]{active_txn_undo_.try_emplace(txn_id)};
        if (!inserted) { prev_txn_undo.emplace(txn_id_it->second); }

        record_t<Key> rec{
            .txn_id            = version_txn_id,
            .is_timestamp      = is_timestamp,
            .is_deleted        = is_deleted,
            .prev_undo_ptr     = actual_prev,
            .prev_txn_undo_ptr = prev_txn_undo,
            .key               = key,
            .op                = op,
            .payload_size      = static_cast<u32>(payload.size_bytes()),
        };

        gsl::span dest{guard.get()->data() + offset, sizeof(record_t<Key>)};
        std::memcpy(dest.data(), &rec, dest.size_bytes());
        if (!payload.empty()) {
            dest = {dest.data() + sizeof(record_t<Key>), payload.size_bytes()};
            std::memcpy(dest.data(), payload.data(), dest.size_bytes());
        }

        header->free_space_ptr += static_cast<u16>(rec_size);
        guard.mark_dirty();
        return txn_id_it->second = {pid, offset};
    }

    // Reads an undo record from a specific pointer
    [[nodiscard]] auto read_record(ptr_t ptr, std::vector<std::byte>& buf)
        -> result<record_t<Key>> {
        std::lock_guard  lock{mutex_};
        auto             guard{TRY(pool_.fetch_read(ptr.page_id))};
        const std::byte* src{guard.get()->data() + ptr.offset};

        record_t<Key> rec;
        std::memcpy(&rec, src, sizeof(record_t<Key>));

        buf.resize(rec.payload_size);
        if (rec.payload_size > 0) {
            std::memcpy(buf.data(), src + sizeof(record_t<Key>), buf.size());
        }
        return rec;
    }

    [[nodiscard]] auto get_last_txn_undo(txn::id_t txn_id) const -> stdx::option<ptr_t> {
        std::lock_guard lock{mutex_};
        if (auto it = active_txn_undo_.find(txn_id); it != active_txn_undo_.end()) {
            return it->second;
        }
        return stdx::none;
    }

    auto remove_txn(txn::id_t txn_id) -> void {
        std::lock_guard lock{mutex_};
        active_txn_undo_.erase(txn_id);
    }

    [[nodiscard]] auto active_page_count() const noexcept -> usize {
        std::lock_guard lock{mutex_};
        return page_active_records_.size();
    }

    [[nodiscard]] auto get_page_active_records(storage::page_id_t pid) const noexcept
        -> stdx::option<u32> {
        std::lock_guard lock{mutex_};
        if (auto it{page_active_records_.find(pid)}; it != page_active_records_.end()) {
            return it->second;
        }
        return stdx::none;
    }

    [[nodiscard]] auto reclaim_prev_ptr(ptr_t ptr) -> result<void> {
        std::lock_guard lock{mutex_};
        auto            guard{TRY(pool_.fetch_write(ptr.page_id))};
        gsl::span       src{guard.get()->data() + ptr.offset, sizeof(record_t<Key>)};

        record_t<Key> rec;
        std::memcpy(&rec, src.data(), src.size_bytes());

        auto old_prev{rec.prev_undo_ptr};
        if (!old_prev) { return {}; } // Already reclaimed

        rec.prev_undo_ptr.reset();
        std::memcpy(src.data(), &rec, src.size_bytes());
        guard.mark_dirty();

        if (old_prev) { reclaim_undo_chain_locked(*old_prev, &record_t<Key>::prev_undo_ptr); }
        return {};
    }

    auto reclaim_undo_chain(ptr_t start_ptr) -> void {
        std::lock_guard lock{mutex_};
        reclaim_undo_chain_locked(start_ptr, &record_t<Key>::prev_undo_ptr);
    }

  private:
    template <typename Proj> auto reclaim_undo_chain_locked(ptr_t start_ptr, Proj proj) -> void {
        stdx::option<ptr_t> cur{start_ptr};
        while (cur) {
            stdx::option<ptr_t> next;
            auto                page_id{cur->page_id};
            {
                // Scope to drop page automatically
                auto bucket_res{pool_.fetch_read(page_id)};
                if (!bucket_res) { break; }

                record_t<Key>   rec;
                const gsl::span src{bucket_res.value().get()->data() + cur->offset,
                                    sizeof(record_t<Key>)};
                std::memcpy(&rec, src.data(), src.size_bytes());
                next = std::invoke(proj, rec);
            }

            if (auto it{page_active_records_.find(page_id)}; it != page_active_records_.end()) {
                if (--it->second == 0) {
                    page_active_records_.erase(it);
                    if (active_page_id_ && *active_page_id_ == page_id) { active_page_id_.reset(); }
                    DISCARD(pool_.delete_page(page_id));
                }
            }
            cur = next;
        }
    }

  private:
    mutable std::mutex                mutex_;
    storage::buffer_pool<PoolSize>&   pool_;
    stdx::option<storage::page_id_t>  active_page_id_;
    stdx::option<const txn::manager&> txn_mgr_;

    active_txn_undo_map_t     active_txn_undo_;
    page_active_records_map_t page_active_records_;
};

} // namespace cairn::txn::undo
