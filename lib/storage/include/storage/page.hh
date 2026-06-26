#pragma once

#include <algorithm>
#include <cstddef>

#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

namespace cairn::storage {

// Logical identifier of a page with the backing file. Page 0 is reserved for metadata
enum class page_id_t : i64 {};

constexpr page_id_t INVALID_PAGE_ID{-1};

constexpr usize DB_PAGE_SIZE{stdx::sizes::kib(8UZ)};

class page {
  public:
    constexpr page() noexcept = default;
    ~page()                   = default;
    MAKE_PINNED(page);

    [[nodiscard]] constexpr auto data(this auto&& self) noexcept -> auto* { return self.data_; }
    [[nodiscard]] constexpr auto page_id() const noexcept -> page_id_t { return page_id_; }

    // Rebinds the page to a fresh id and clears its state
    auto reset(page_id_t pid) noexcept -> void {
        page_id_ = pid;
        std::fill_n(data_, DB_PAGE_SIZE, std::byte{0});
    }

  private:
    alignas(std::max_align_t) std::byte data_[DB_PAGE_SIZE]{};

    page_id_t page_id_{INVALID_PAGE_ID};
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
