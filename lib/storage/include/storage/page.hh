#pragma once

#include <algorithm>
#include <cstddef>

#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/rw_latch.hh"
#include "wal/sequence_number.hh"

namespace cairn::storage {

// Logical identifier of a page with the backing file. Page 0 is reserved for metadata
enum class page_id_t : i64 {};
constexpr page_id_t INVALID_PAGE_ID{-1};

constexpr usize DB_PAGE_SIZE{stdx::sizes::kib(8UZ)};

// The most primitive thread-safe abstraction over a chunk of memory
class page {
  public:
    constexpr page() noexcept = default;
    ~page()                   = default;
    MAKE_PINNED(page);

    [[nodiscard]] constexpr auto data(this auto&& self) noexcept -> auto* { return self.data_; }
    [[nodiscard]] constexpr auto page_id() const noexcept -> page_id_t { return page_id_; }
    [[nodiscard]] constexpr auto pin_count() const noexcept -> i32 { return pin_count_; }
    [[nodiscard]] constexpr auto is_dirty() const noexcept -> bool { return is_dirty_; }
    [[nodiscard]] auto           latch() noexcept -> rw_latch& { return latch_; }
    [[nodiscard]] constexpr auto page_lsn() const noexcept -> stdx::option<wal::lsn_t> {
        return page_lsn_;
    }

    // Rebinds the page to a fresh id and clears its state
    constexpr auto reset(page_id_t pid) noexcept -> void {
        std::fill_n(data_, DB_PAGE_SIZE, std::byte{0});
        page_id_   = pid;
        pin_count_ = 0;
        is_dirty_  = false;
        page_lsn_.reset();
        rec_lsn_.reset();
    }

    constexpr auto set_page_lsn(stdx::option<wal::lsn_t> lsn) noexcept -> void {
        page_lsn_ = lsn;
        if (lsn && !rec_lsn_) { rec_lsn_ = lsn; }
    }
    constexpr auto               set_dirty(bool dirty) noexcept -> void { is_dirty_ = dirty; }
    constexpr auto               pin() noexcept -> i32 { return ++pin_count_; }
    constexpr auto               unpin() noexcept -> i32 { return --pin_count_; }
    [[nodiscard]] constexpr auto rec_lsn() const noexcept -> stdx::option<wal::lsn_t> {
        return rec_lsn_;
    }
    constexpr auto clear_rec_lsn() noexcept -> void { rec_lsn_.reset(); }

  private:
    alignas(std::max_align_t) std::byte data_[DB_PAGE_SIZE]{};
    rw_latch latch_;

    page_id_t                page_id_{INVALID_PAGE_ID};
    i32                      pin_count_{0};
    bool                     is_dirty_{false};
    stdx::option<wal::lsn_t> page_lsn_{stdx::none};
    stdx::option<wal::lsn_t> rec_lsn_{stdx::none};
};

} // namespace cairn::storage

namespace stdx {

template <> struct nullable<cairn::storage::page_id_t> {
    using pid_t = cairn::storage::page_id_t;
    [[nodiscard]] static constexpr auto invalid() noexcept -> pid_t {
        return cairn::storage::INVALID_PAGE_ID;
    }

    [[nodiscard]] static constexpr auto is_valid(const pid_t& id) noexcept -> bool {
        return id != invalid();
    }
};

} // namespace stdx
