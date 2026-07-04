#pragma once

#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

#include "storage/page.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "wal/sequence_number.hh"

namespace cairn {

namespace wal { class manager; } // namespace wal

namespace storage {

enum class slot_id_t : i32 {};
constexpr slot_id_t INVALID_SLOT_ID{-1};

// Uses the default specialization of stdx::option (compact) since u16 exceeds PAGE_SIZE
enum class slot_size_t : u16 {};
constexpr auto MAX_SLOT_SIZE{std::numeric_limits<std::underlying_type_t<slot_size_t>>::max()};

struct log_update_params_t {
    txn::id_t                   txn_id{txn::INVALID_TXN_ID};
    stdx::option<wal::lsn_t>        prev_lsn;
    stdx::option<wal::manager&> log;
};

class slotted_page {
  public:
    explicit slotted_page(page& p) noexcept : page_{p} {}

    // Zeroes out the page's internal header
    auto refresh_page() noexcept -> void;

    [[nodiscard]] auto insert(gsl::span<const std::byte> tuple, log_update_params_t log_params = {})
        -> result<slot_id_t>;
    [[nodiscard]] auto get(slot_id_t id) const -> result<gsl::span<const std::byte>>;
    [[nodiscard]] auto update(slot_id_t                  id,
                              gsl::span<const std::byte> tuple,
                              log_update_params_t        log_params = {}) -> result<void>;
    [[nodiscard]] auto remove(slot_id_t id, log_update_params_t log_params = {}) -> result<void>;
    auto               compact() noexcept -> void;

    [[nodiscard]] auto slot_count() const noexcept -> i32;
    [[nodiscard]] auto free_space() const noexcept -> i32;

  private:
    struct slot_t {
        u16                       offset;
        stdx::option<slot_size_t> size;
    };

    struct header_t {
        i32 slot_count{0};
        i32 free_space_ptr{DB_PAGE_SIZE};
        i32 deleted_slot_count{0};
    };

  private:
    template <stdx::NumericIntegral I = usize>
    static constexpr auto SLOT_SIZE{static_cast<I>(sizeof(slot_t))};
    static_assert(SLOT_SIZE<> == 4);

    template <stdx::NumericIntegral I = usize>
    static constexpr auto HEADER_SIZE{static_cast<I>(sizeof(header_t))};
    static_assert(HEADER_SIZE<> == 12);

    static constexpr usize MAXIMUM_SLOTS{(DB_PAGE_SIZE - HEADER_SIZE<>) / (SLOT_SIZE<> + 1)};

  private:
    template <typename Self>
    [[nodiscard]] auto get_raw(this Self&& self, slot_id_t id)
        -> result<std::pair<gsl::not_null<stdx::const_dispatch_t<Self, header_t>*>,
                            gsl::not_null<stdx::const_dispatch_t<Self, slot_t>*>>> {
        gsl::not_null header{self.as_header()};
        if (id < slot_id_t{0} || std::to_underlying(id) >= header->slot_count) {
            return stdx::err{error_t::STORAGE_INVALID_SLOT};
        }

        gsl::not_null slot{&self.as_slots()[static_cast<usize>(id)]};
        if (!slot->size) { return stdx::err{error_t::STORAGE_TUPLE_DELETED}; }
        return std::make_pair(header, slot);
    }

    template <typename Self> [[nodiscard]] auto as_header(this Self&& self) noexcept {
        using to = stdx::const_dispatch_t<Self, header_t>;
        return gsl::not_null{reinterpret_cast<to*>(self.page_->data())};
    }

    template <typename Self> [[nodiscard]] auto as_slots(this Self&& self) noexcept {
        using to = stdx::const_dispatch_t<Self, slot_t>;
        return gsl::span{reinterpret_cast<to*>(self.page_->data() + sizeof(header_t)),
                         static_cast<usize>(self.slot_count())};
    }

  private:
    stdx::option<page&> page_;
};

} // namespace storage

} // namespace cairn
