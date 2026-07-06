#pragma once

#include <cstddef>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "support/error.hh"
#include "txn/id.hh"

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
    txn::id_t           txn_id{INVALID_TXN_ID};
    bool                is_timestamp{false};
    bool                is_deleted{false};
    stdx::option<ptr_t> prev_undo_ptr;
    stdx::option<ptr_t> prev_txn_undo_ptr;
    Key                 key;
    op_t                op{op_t::INSERT};
    u32                 payload_size{0};
};

struct page_header_t {
    u16 free_space_ptr{sizeof(page_header_t)};
};

template <typename Key> struct undo_record_t {
    record_t<Key>          record;
    std::vector<std::byte> payload;
};

template <typename Key, usize PoolSize> class manager {
  public:
    explicit manager(storage::buffer_pool<PoolSize>& pool) noexcept : pool_{pool} {}
    ~manager() = default;

    // Appends an undo record sequentially to the active undo page, allocating if needed
    [[nodiscard]] auto append_record(txn::id_t                  txn_id,
                                     const Key&                 key,
                                     op_t                       op,
                                     bool                       is_timestamp,
                                     bool                       is_deleted,
                                     txn::id_t                  version_txn_id,
                                     stdx::option<ptr_t>        prev_undo_ptr,
                                     gsl::span<const std::byte> payload) -> result<ptr_t> {
        std::lock_guard lock{mutex_};
        const usize     rec_size{sizeof(record_t<Key>) + payload.size_bytes()};
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
        auto       guard{TRY(pool_.fetch_write(pid))};
        auto       header{guard.template as<page_header_t>()};
        const u16  offset{header->free_space_ptr};

        stdx::option<ptr_t> prev_txn_undo;
        auto [txn_id_it, inserted]{active_txn_undo_.try_emplace(txn_id)};
        if (!inserted) { prev_txn_undo.emplace(txn_id_it->second); }

        record_t<Key> rec{
            .txn_id            = version_txn_id,
            .is_timestamp      = is_timestamp,
            .is_deleted        = is_deleted,
            .prev_undo_ptr     = prev_undo_ptr,
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
    [[nodiscard]] auto read_record(ptr_t ptr) -> result<undo_record_t<Key>> {
        std::lock_guard  lock{mutex_};
        auto             guard{TRY(pool_.fetch_read(ptr.page_id))};
        const std::byte* src{guard.get()->data() + ptr.offset};

        record_t<Key> rec;
        std::memcpy(&rec, src, sizeof(record_t<Key>));

        std::vector<std::byte> payload(rec.payload_size);
        if (rec.payload_size > 0) {
            std::memcpy(payload.data(), src + sizeof(record_t<Key>), rec.payload_size);
        }

        return undo_record_t<Key>{.record = rec, .payload = std::move(payload)};
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

  private:
    storage::buffer_pool<PoolSize>&  pool_;
    stdx::option<storage::page_id_t> active_page_id_;

    mutable std::mutex                                             mutex_;
    ankerl::unordered_dense::map<txn::id_t, ptr_t, txn::id_hash_t> active_txn_undo_;
};

} // namespace cairn::txn::undo
