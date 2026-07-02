#pragma once

#include <cstddef>
#include <utility>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

#include "storage/error.hh"
#include "storage/page.hh"

namespace cairn::storage {

enum class slot_id_t : i32 {};

constexpr slot_id_t INVALID_SLOT_ID{-1};

enum class slot_size_t : u16 {};

} // namespace cairn::storage

namespace stdx {

template <> struct nullable<cairn::storage::slot_size_t> {
    using ss_t = cairn::storage::slot_size_t;
    [[nodiscard]] static constexpr auto invalid() noexcept -> ss_t { return ss_t{0}; }
    [[nodiscard]] static constexpr auto is_valid(const ss_t& id) noexcept -> bool {
        return id != invalid();
    }
};

} // namespace stdx

namespace cairn::storage {

class slotted_page {
  public:
    explicit slotted_page(page& p) noexcept : page_{p} {}

    // Zeroes out the page's internal header
    auto refresh_page() noexcept -> void;

    [[nodiscard]] auto insert(gsl::span<const std::byte> tuple) -> result<slot_id_t>;
    [[nodiscard]] auto get(slot_id_t id) const -> result<gsl::span<const std::byte>>;
    [[nodiscard]] auto update(slot_id_t id, gsl::span<const std::byte> tuple) -> result<void>;
    [[nodiscard]] auto remove(slot_id_t id) -> result<void>;
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
            return stdx::err{error_t::INVALID_SLOT};
        }

        gsl::not_null slot{&self.as_slots()[static_cast<usize>(id)]};
        if (!slot->size) { return stdx::err{error_t::TUPLE_DELETED}; }
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

} // namespace cairn::storage
