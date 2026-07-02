#include "storage/slotted_page.hh"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/fixed/vector.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/error.hh"
#include "storage/page.hh"

namespace cairn::storage {

auto slotted_page::refresh_page() noexcept -> void {
    auto header{as_header()};
    *header = header_t{};
}

auto slotted_page::insert(gsl::span<const std::byte> tuple) -> result<slot_id_t> {
    PROFILE_FUNCTION();
    auto       header{as_header()};
    const auto i_tuple_size{static_cast<i32>(tuple.size())};
    i32        required_space{i_tuple_size};
    slot_id_t  id{INVALID_SLOT_ID};

    // Reusing a tombstone is always more efficient
    if (header->deleted_slot_count > 0) {
        for (i32 i{0}; i < header->slot_count; ++i) {
            if (!as_slots()[static_cast<usize>(i)].size) {
                id = slot_id_t{i};
                break;
            }
        }
    }

    if (id == INVALID_SLOT_ID) { required_space += SLOT_SIZE<i32>; }
    if (free_space() < required_space) {
        compact();
        if (free_space() < required_space) { return stdx::err{error_t::PAGE_FULL}; }
    }

    if (id == INVALID_SLOT_ID) {
        id = slot_id_t{header->slot_count++};
    } else {
        header->deleted_slot_count--;
    }

    header->free_space_ptr -= i_tuple_size;
    std::copy_n(tuple.data(), tuple.size(), page_->data() + header->free_space_ptr);

    const auto slots{as_slots()};
    const auto u_id{static_cast<usize>(id)};
    slots[u_id].size.emplace(static_cast<u16>(tuple.size()));
    slots[u_id].offset = static_cast<u16>(header->free_space_ptr);
    return id;
}

auto slotted_page::get(slot_id_t id) const -> result<gsl::span<const std::byte>> {
    const auto [_, slot] = TRY(get_raw(id));
    return gsl::span{page_->data() + slot->offset, std::to_underlying(*slot->size)};
}

auto slotted_page::update(slot_id_t id, gsl::span<const std::byte> tuple) -> result<void> {
    PROFILE_FUNCTION();
    const auto [header, slot] = TRY(get_raw(id));
    const auto slot_size{std::to_underlying(*slot->size)};
    if (tuple.size() < slot_size) {
        std::copy_n(tuple.data(), tuple.size(), page_->data() + slot->offset);
        slot->size.emplace(static_cast<u16>(tuple.size()));
        return {};
    }

    // We need more space
    const auto old_offset{slot->offset};
    slot->size.reset();
    header->deleted_slot_count++;

    // Compacting might open up enough space for the tuple to pack in
    if (static_cast<usize>(free_space()) < tuple.size()) {
        compact();

        // Still cannot fit the tuple :(
        if (static_cast<usize>(free_space()) < tuple.size()) {
            slot->size.emplace(slot_size);
            slot->offset = old_offset;
            header->deleted_slot_count--;
            return stdx::err{error_t::PAGE_FULL};
        }
    }

    header->deleted_slot_count--;
    header->free_space_ptr -= static_cast<i32>(tuple.size());
    std::copy_n(tuple.data(), tuple.size(), page_->data() + header->free_space_ptr);

    slot->offset = static_cast<u16>(header->free_space_ptr);
    slot->size.emplace(static_cast<u16>(tuple.size()));
    return {};
}

auto slotted_page::remove(slot_id_t id) -> result<void> {
    PROFILE_FUNCTION();
    const auto [header, slot] = TRY(get_raw(id));
    slot->size.reset();
    header->deleted_slot_count++;
    return {};
}

auto slotted_page::compact() noexcept -> void {
    PROFILE_FUNCTION();
    auto header{as_header()};
    auto slots{as_slots()};

    // TODO(tcs): Maybe bake in sorting with emplace back calls
    stdx::fixed::vector<usize, MAXIMUM_SLOTS> active_slot_indexes;
    for (usize i{0}; i < static_cast<usize>(header->slot_count); ++i) {
        if (slots[i].size) { active_slot_indexes.emplace_back(i); }
    }

    // This makes me feel gross
    {
        PROFILE_SCOPE("slotted page offset sorting");
        const auto offset_descending = [&](usize a, usize b) {
            return slots[a].offset > slots[b].offset;
        };
        std::ranges::sort(active_slot_indexes, offset_descending);
    }

    u16 current_offset{DB_PAGE_SIZE};
    for (auto idx : active_slot_indexes) {
        auto&      slot_offset{slots[idx].offset};
        const auto slot_size{std::to_underlying(*slots[idx].size)};
        current_offset -= slot_size;

        if (slot_offset != current_offset) {
            std::copy_backward(page_->data() + slot_offset,
                               page_->data() + slot_offset + slot_size,
                               page_->data() + current_offset + slot_size);
            slot_offset = current_offset;
        }
    }
    header->free_space_ptr = current_offset;
}

auto slotted_page::slot_count() const noexcept -> i32 { return as_header()->slot_count; }

auto slotted_page::free_space() const noexcept -> i32 {
    const auto header{as_header()};
    return header->free_space_ptr - (HEADER_SIZE<i32> + header->slot_count * SLOT_SIZE<i32>);
}

} // namespace cairn::storage
