#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

#include "storage/bplus_internal/traits.hh"
#include "storage/page.hh"

namespace cairn::storage::detail {

template <BPlusNodePayload Key, usize MaxTupleSize> struct iot_leaf_trait {
    using Value = gsl::span<const std::byte>;

    struct slot_t {
        Key key;
        u16 offset;
        u16 size;
    };

    struct node_type {
        node_kind               type;
        std::array<u8, 3>       pad_;
        i32                     size;
        stdx::option<page_id_t> next;
        u16                     free_space_ptr;
        std::array<u8, 6>       pad2_;

        template <typename Self> [[nodiscard]] auto get_slots(this Self&& self) noexcept {
            return gsl::span{reinterpret_cast<stdx::const_dispatch_t<Self, slot_t>*>(
                                 reinterpret_cast<stdx::const_dispatch_t<Self, std::byte>*>(&self) +
                                 sizeof(node_type)),
                             static_cast<usize>(self.size)};
        }
    };

    static constexpr usize NODE_PREFIX{sizeof(node_type)};
    static constexpr usize SLOTS{(DB_PAGE_SIZE - NODE_PREFIX) / (sizeof(slot_t) + MaxTupleSize)};
    static constexpr i32   CAP{static_cast<i32>(SLOTS)};
    static constexpr i32   MIN{CAP / 2};

    static auto as_node(page_ptr_t p) noexcept {
        return gsl::not_null{reinterpret_cast<node_type*>(p->data())};
    }

    static auto as_node(const_page_ptr_t p) noexcept -> gsl::not_null<const node_type*> {
        return gsl::not_null{reinterpret_cast<const node_type*>(p->data())};
    }

    static auto init(page_ptr_t p) noexcept -> void {
        auto n{as_node(p)};
        n->type = node_kind::LEAF;
        n->size = 0;
        n->next.reset();
        n->free_space_ptr = static_cast<u16>(DB_PAGE_SIZE);
    }

    [[nodiscard]] static auto size(const_page_ptr_t p) noexcept -> i32 { return as_node(p)->size; }

    [[nodiscard]] static auto get_key(const_page_ptr_t p, i32 idx) noexcept -> Key {
        return as_node(p)->get_slots()[static_cast<usize>(idx)].key;
    }

    [[nodiscard]] static auto get_value(const_page_ptr_t p, i32 idx) noexcept -> Value {
        auto        n{as_node(p)};
        const auto& slot{n->get_slots()[static_cast<usize>(idx)]};
        return gsl::span{p->data() + slot.offset, slot.size};
    }

    static auto set_key(page_ptr_t p, i32 idx, const Key& key) noexcept -> void {
        as_node(p)->get_slots()[static_cast<usize>(idx)].key = key;
    }

    [[nodiscard]] static auto next(const_page_ptr_t p) noexcept -> stdx::option<page_id_t> {
        return as_node(p)->next;
    }

    [[nodiscard]] static auto can_emplace(const_page_ptr_t p, const Key&, const Value& val) noexcept
        -> bool {
        auto n{as_node(p)};
        if (n->size >= CAP) { return false; }

        const usize required_space{sizeof(slot_t) + val.size_bytes()};
        usize       used_data_bytes{0};
        for (const auto& slot : n->get_slots()) { used_data_bytes += slot.size; }

        const usize logical_free{
            DB_PAGE_SIZE -
            (NODE_PREFIX + (static_cast<usize>(n->size) * sizeof(slot_t)) + used_data_bytes)};
        return logical_free >= required_space;
    }

    [[nodiscard]] static auto can_remove(const_page_ptr_t p) noexcept -> bool {
        return size(p) > MIN;
    }

    static auto compact(gsl::not_null<node_type*> n, page_ptr_t p) noexcept -> void {
        std::array<std::byte, DB_PAGE_SIZE> temp_buf;
        usize                               current_offset{DB_PAGE_SIZE};

        auto slots{n->get_slots()};
        for (auto& slot : slots) {
            current_offset -= slot.size;
            std::copy_n(p->data() + slot.offset, slot.size, temp_buf.data() + current_offset);
            slot.offset = static_cast<u16>(current_offset);
        }

        std::copy_n(temp_buf.data() + current_offset,
                    DB_PAGE_SIZE - current_offset,
                    p->data() + current_offset);
        n->free_space_ptr = static_cast<u16>(current_offset);
    }

    static auto emplace_at(page_ptr_t p, i32 idx, const Key& key, const Value& val) noexcept
        -> void {
        auto       n{as_node(p)};
        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{static_cast<usize>(n->size)};

        const usize required_space{sizeof(slot_t) + val.size_bytes()};
        const usize physical_free{n->free_space_ptr - (NODE_PREFIX + (u_size * sizeof(slot_t)))};
        if (physical_free < required_space) { compact(n, p); }

        gsl::span slots{
            reinterpret_cast<slot_t*>(reinterpret_cast<std::byte*>(n.get()) + sizeof(node_type)),
            u_size + 1};

        if (u_idx < u_size) {
            std::copy_backward(
                slots.data() + u_idx, slots.data() + u_size, slots.data() + u_size + 1);
        }

        n->free_space_ptr -= static_cast<u16>(val.size_bytes());

        slots[u_idx].key    = key;
        slots[u_idx].offset = n->free_space_ptr;
        slots[u_idx].size   = static_cast<u16>(val.size_bytes());

        std::copy_n(val.data(), val.size_bytes(), p->data() + n->free_space_ptr);
        n->size += 1;
    }

    static auto remove_at(page_ptr_t p, i32 idx) noexcept -> void {
        auto       n{as_node(p)};
        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{static_cast<usize>(n->size)};

        auto slots{n->get_slots()};
        if (u_idx + 1 < u_size) {
            std::copy(slots.data() + u_idx + 1, slots.data() + u_size, slots.data() + u_idx);
        }
        n->size -= 1;
    }

    static auto split(page_ptr_t   left_p,
                      page_ptr_t   right_p,
                      page_id_t    right_pid,
                      i32          idx,
                      const Key&   key,
                      const Value& val) -> Key {
        auto l{as_node(left_p)};

        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{static_cast<usize>(l->size)};

        std::array<slot_t, CAP + 1> tmp_slots;
        auto                        left_slots{l->get_slots()};

        std::copy_n(left_slots.data(), u_idx, tmp_slots.data());

        tmp_slots[u_idx].key    = key;
        tmp_slots[u_idx].offset = 0;
        tmp_slots[u_idx].size   = static_cast<u16>(val.size_bytes());

        if (u_idx < u_size) {
            std::copy_n(left_slots.data() + u_idx, u_size - u_idx, tmp_slots.data() + u_idx + 1);
        }

        std::array<std::byte, DB_PAGE_SIZE> temp_data;
        std::copy_n(left_p->data(), DB_PAGE_SIZE, temp_data.data());

        static constexpr i32 total{CAP + 1};
        static constexpr i32 left_count{(total + 1) / 2};
        static constexpr i32 right_count{total - left_count};

        init(right_p);
        as_node(right_p)->next = l->next;

        for (i32 i{0}; i < right_count; ++i) {
            const auto& s{tmp_slots[static_cast<usize>(left_count + i)]};
            emplace_at(right_p,
                       i,
                       s.key,
                       Value{(left_count + i == idx)
                                 ? val
                                 : gsl::span{temp_data.data() + s.offset, s.size}});
        }

        init(left_p);
        as_node(left_p)->next = right_pid;

        for (i32 i{0}; i < left_count; ++i) {
            const auto& s{tmp_slots[static_cast<usize>(i)]};
            emplace_at(left_p,
                       i,
                       s.key,
                       (i == idx) ? val : gsl::span{temp_data.data() + s.offset, s.size});
        }

        return as_node(right_p)->get_slots()[0].key;
    }

    static auto merge_into_left(page_ptr_t left_p, page_ptr_t right_p, Key& /*sep*/) noexcept
        -> void {
        auto       r{as_node(right_p)};
        const auto r_size{r->size};

        for (i32 i{0}; i < r_size; ++i) {
            emplace_at(left_p, size(left_p), get_key(right_p, i), get_value(right_p, i));
        }
        as_node(left_p)->next = r->next;
    }

    static auto borrow_from_left(page_ptr_t left_p, page_ptr_t node_p, Key& sep) noexcept -> void {
        const i32 l_last{size(left_p) - 1};

        emplace_at(node_p, 0, get_key(left_p, l_last), get_value(left_p, l_last));
        remove_at(left_p, l_last);

        sep = get_key(left_p, size(left_p) - 1);
    }

    static auto borrow_from_right(page_ptr_t right_p, page_ptr_t node_p, Key& sep) noexcept
        -> void {
        emplace_at(node_p, size(node_p), get_key(right_p, 0), get_value(right_p, 0));
        remove_at(right_p, 0);
        sep = get_key(right_p, 0);
    }
};

} // namespace cairn::storage::detail
