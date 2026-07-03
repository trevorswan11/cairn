#pragma once

#include <algorithm>
#include <array>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

#include "storage/bplus_internal/traits.hh"
#include "storage/page.hh"

namespace cairn::storage::detail {

template <BPlusNodePayload Key, BPlusNodePayload Value> struct default_leaf_trait {
    static constexpr usize NODE_PREFIX{16}; // kind+pad+size (8) + next (8)
    static constexpr usize SLOTS{(DB_PAGE_SIZE - NODE_PREFIX) / (sizeof(Key) + sizeof(Value))};
    static_assert(SLOTS >= 2, "key/value too large for an 8 KiB leaf");

    struct node_type {
        node_kind                type;
        std::array<u8, 3>        pad_;
        i32                      size;
        stdx::option<page_id_t>  next;
        std::array<Key, SLOTS>   keys;
        std::array<Value, SLOTS> values;

        [[nodiscard]] auto get_size() const noexcept -> usize { return static_cast<usize>(size); }
    };
    static_assert(sizeof(node_type) <= DB_PAGE_SIZE, "leaf node overflows a page");
    static_assert(stdx::StandardLayout<node_type> && stdx::TriviallyCopyable<node_type>);

    static constexpr i32 CAP{static_cast<i32>(SLOTS)};
    static constexpr i32 MIN{CAP / 2};

    [[nodiscard]] static auto as_node(page_ptr_t p) noexcept {
        return gsl::not_null{reinterpret_cast<node_type*>(p->data())};
    }

    [[nodiscard]] static auto as_node(const_page_ptr_t p) noexcept {
        return gsl::not_null{reinterpret_cast<const node_type*>(p->data())};
    }

    static auto init(page_ptr_t p) noexcept -> void {
        auto n{as_node(p)};
        n->type = node_kind::LEAF;
        n->size = 0;
        n->next.reset();
    }

    [[nodiscard]] static auto size(const_page_ptr_t p) noexcept -> i32 { return as_node(p)->size; }

    [[nodiscard]] static auto get_key(const_page_ptr_t p, i32 idx) noexcept -> Key {
        return as_node(p)->keys[static_cast<usize>(idx)];
    }

    [[nodiscard]] static auto get_value(const_page_ptr_t p, i32 idx) noexcept -> Value {
        return as_node(p)->values[static_cast<usize>(idx)];
    }

    static auto set_key(page_ptr_t p, i32 idx, Key key) noexcept -> void {
        as_node(p)->keys[static_cast<usize>(idx)] = key;
    }

    [[nodiscard]] static auto next(const_page_ptr_t p) noexcept -> stdx::option<page_id_t> {
        return as_node(p)->next;
    }

    [[nodiscard]] static auto
    can_emplace(const_page_ptr_t p, const Key& /*key*/, const Value& /*value*/) noexcept -> bool {
        return size(p) < CAP;
    }

    [[nodiscard]] static auto can_remove(const_page_ptr_t p) noexcept -> bool {
        return size(p) > MIN;
    }

    static auto emplace_at(page_ptr_t p, i32 idx, const Key& key, const Value& value) noexcept
        -> void {
        auto       n{as_node(p)};
        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{n->get_size()};

        if (u_idx < u_size) {
            std::copy_backward(
                n->keys.data() + u_idx, n->keys.data() + u_size, n->keys.data() + u_size + 1);
            std::copy_backward(
                n->values.data() + u_idx, n->values.data() + u_size, n->values.data() + u_size + 1);
        }
        n->keys[u_idx]   = key;
        n->values[u_idx] = value;
        n->size += 1;
    }

    static auto remove_at(page_ptr_t p, i32 idx) noexcept -> void {
        auto       n{as_node(p)};
        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{n->get_size()};
        std::move(n->keys.data() + u_idx + 1, n->keys.data() + u_size, n->keys.data() + u_idx);
        std::move(
            n->values.data() + u_idx + 1, n->values.data() + u_size, n->values.data() + u_idx);
        n->size -= 1;
    }

    [[nodiscard]] static auto split(page_ptr_t   left_p,
                                    page_ptr_t   right_p,
                                    page_id_t    right_pid,
                                    i32          idx,
                                    const Key&   key,
                                    const Value& value) noexcept -> Key {
        auto left{as_node(left_p)};
        auto right{as_node(right_p)};

        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{left->get_size()};

        std::array<Key, SLOTS + 1>   tmp_keys;
        std::array<Value, SLOTS + 1> tmp_vals;

        std::copy(left->keys.data(), left->keys.data() + u_idx, tmp_keys.data());
        std::copy(left->values.data(), left->values.data() + u_idx, tmp_vals.data());

        tmp_keys[u_idx] = key;
        tmp_vals[u_idx] = value;

        if (u_idx < u_size) {
            std::copy(
                left->keys.data() + u_idx, left->keys.data() + u_size, tmp_keys.data() + u_idx + 1);
            std::copy(left->values.data() + u_idx,
                      left->values.data() + u_size,
                      tmp_vals.data() + u_idx + 1);
        }

        static constexpr i32 total{CAP + 1};
        static constexpr i32 left_count{(total + 1) / 2};

        right->type = node_kind::LEAF;
        right->size = total - left_count;
        right->next = left->next;

        const auto u_left_count{static_cast<usize>(left_count)};
        const auto u_right_size{right->get_size()};

        std::copy(tmp_keys.data() + u_left_count,
                  tmp_keys.data() + u_left_count + u_right_size,
                  right->keys.data());
        std::copy(tmp_vals.data() + u_left_count,
                  tmp_vals.data() + u_left_count + u_right_size,
                  right->values.data());

        std::copy(tmp_keys.data(), tmp_keys.data() + u_left_count, left->keys.data());
        std::copy(tmp_vals.data(), tmp_vals.data() + u_left_count, left->values.data());

        left->size = left_count;
        left->next = right_pid;
        return right->keys[0];
    }

    static auto merge_into_left(page_ptr_t left_p, page_ptr_t right_p, Key&) noexcept -> void {
        auto l{as_node(left_p)};
        auto r{as_node(right_p)};

        const auto l_size{l->get_size()};
        const auto r_size{r->get_size()};

        std::copy_n(r->keys.data(), r_size, l->keys.data() + l_size);
        std::copy_n(r->values.data(), r_size, l->values.data() + l_size);
        l->size += r->size;
        l->next = r->next;
    }

    static auto borrow_from_left(page_ptr_t left_p, page_ptr_t node_p, Key& parent_sep) noexcept
        -> void {
        auto l{as_node(left_p)};
        auto n{as_node(node_p)};

        const auto l_size{l->get_size()};
        const auto u_size{n->get_size()};

        std::copy_backward(n->keys.data(), n->keys.data() + u_size, n->keys.data() + u_size + 1);
        std::copy_backward(
            n->values.data(), n->values.data() + u_size, n->values.data() + u_size + 1);

        n->keys[0]   = l->keys[l_size - 1];
        n->values[0] = l->values[l_size - 1];
        n->size += 1;

        l->size -= 1;
        parent_sep = n->keys[0];
    }

    static auto borrow_from_right(page_ptr_t right_p, page_ptr_t node_p, Key& parent_sep) noexcept
        -> void {
        auto n{as_node(node_p)};
        auto r{as_node(right_p)};

        const auto u_size{n->get_size()};
        const auto r_size{r->get_size()};

        n->keys[u_size]   = r->keys[0];
        n->values[u_size] = r->values[0];
        n->size += 1;

        std::move(r->keys.data() + 1, r->keys.data() + r_size, r->keys.data());
        std::move(r->values.data() + 1, r->values.data() + r_size, r->values.data());
        r->size -= 1;

        parent_sep = r->keys[0];
    }
};

template <BPlusNodePayload Key> struct default_internal_trait {
    static constexpr usize NODE_PREFIX{8}; // kind+pad+size
    static constexpr usize SLOTS{(DB_PAGE_SIZE - NODE_PREFIX - sizeof(page_id_t)) /
                                 (sizeof(Key) + sizeof(page_id_t))};
    static_assert(SLOTS >= 2, "key too large for an 8 KiB internal node");

    struct node_type {
        node_kind                        type;
        std::array<u8, 3>                pad_;
        i32                              size;
        std::array<Key, SLOTS>           keys;
        std::array<page_id_t, SLOTS + 1> children;

        [[nodiscard]] auto get_size() const noexcept -> usize { return static_cast<usize>(size); }
    };
    static_assert(sizeof(node_type) <= DB_PAGE_SIZE, "internal node overflows a page");
    static_assert(stdx::StandardLayout<node_type> && stdx::TriviallyCopyable<node_type>);

    static constexpr i32 CAP{static_cast<i32>(SLOTS)};
    static constexpr i32 MIN{CAP / 2};

    [[nodiscard]] static auto as_node(page_ptr_t p) noexcept {
        return gsl::not_null{reinterpret_cast<node_type*>(p->data())};
    }
    [[nodiscard]] static auto as_node(const_page_ptr_t p) noexcept {
        return gsl::not_null{reinterpret_cast<const node_type*>(p->data())};
    }

    static auto init(page_ptr_t p) noexcept -> void {
        auto n{as_node(p)};
        n->type = node_kind::INTERNAL;
        n->size = 0;
    }

    static auto init_root(page_ptr_t p, Key key, page_id_t left, page_id_t right) noexcept -> void {
        auto n{as_node(p)};
        n->type        = node_kind::INTERNAL;
        n->size        = 1;
        n->keys[0]     = key;
        n->children[0] = left;
        n->children[1] = right;
    }

    [[nodiscard]] static auto size(const_page_ptr_t p) noexcept -> i32 { return as_node(p)->size; }

    [[nodiscard]] static auto get_key(const_page_ptr_t p, i32 idx) noexcept -> Key {
        return as_node(p)->keys[static_cast<usize>(idx)];
    }

    static auto set_key(page_ptr_t p, i32 idx, Key key) noexcept -> void {
        as_node(p)->keys[static_cast<usize>(idx)] = key;
    }

    [[nodiscard]] static auto get_child(const_page_ptr_t p, i32 idx) noexcept -> page_id_t {
        return as_node(p)->children[static_cast<usize>(idx)];
    }

    [[nodiscard]] static auto
    can_emplace(const_page_ptr_t p, const Key& /*key*/, page_id_t /*pid*/) noexcept -> bool {
        return size(p) < CAP;
    }

    [[nodiscard]] static auto can_remove(const_page_ptr_t p) noexcept -> bool {
        return size(p) > MIN;
    }

    static auto emplace_at(page_ptr_t p, i32 idx, const Key& key, page_id_t right_child) noexcept
        -> void {
        auto       n{as_node(p)};
        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{n->get_size()};

        if (u_idx < u_size) {
            std::copy_backward(
                n->keys.data() + u_idx, n->keys.data() + u_size, n->keys.data() + u_size + 1);
            std::copy_backward(n->children.data() + u_idx + 1,
                               n->children.data() + u_size + 1,
                               n->children.data() + u_size + 2);
        }
        n->keys[u_idx]         = key;
        n->children[u_idx + 1] = right_child;
        n->size += 1;
    }

    static auto remove_at(page_ptr_t p, i32 idx) noexcept -> void {
        auto       n{as_node(p)};
        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{n->get_size()};

        if (u_size > u_idx + 1) {
            std::move(n->keys.data() + u_idx + 1, n->keys.data() + u_size, n->keys.data() + u_idx);
            std::move(n->children.data() + u_idx + 2,
                      n->children.data() + u_size + 1,
                      n->children.data() + u_idx + 1);
        }
        n->size -= 1;
    }

    template <typename Comp>
    [[nodiscard]] static auto split(
        page_ptr_t left_p, page_ptr_t right_p, const Key& sep_key, page_id_t right_child, Comp comp)
        -> Key {
        auto left{as_node(left_p)};
        auto right{as_node(right_p)};

        std::array<Key, SLOTS + 1>       tmp_keys;
        std::array<page_id_t, SLOTS + 2> tmp_children;

        const gsl::span active_keys{left->keys.data(), left->get_size()};
        auto            it{std::ranges::lower_bound(active_keys, sep_key, comp)};
        const auto      u_pos{static_cast<usize>(std::distance(active_keys.begin(), it))};
        const auto      u_size{left->get_size()};

        std::copy(left->keys.data(), left->keys.data() + u_pos, tmp_keys.data());
        std::copy(left->children.data(), left->children.data() + u_pos + 1, tmp_children.data());

        tmp_keys[u_pos]         = sep_key;
        tmp_children[u_pos + 1] = right_child;

        if (u_pos < u_size) {
            std::copy(
                left->keys.data() + u_pos, left->keys.data() + u_size, tmp_keys.data() + u_pos + 1);
            std::copy(left->children.data() + u_pos + 1,
                      left->children.data() + u_size + 1,
                      tmp_children.data() + u_pos + 2);
        }

        static constexpr const i32 total_keys{CAP + 1};
        static constexpr const i32 mid{total_keys / 2};

        right->type = node_kind::INTERNAL;
        right->size = total_keys - mid - 1;

        const auto u_mid{static_cast<usize>(mid)};
        const auto u_right_size{right->get_size()};

        std::copy(tmp_keys.data() + u_mid + 1,
                  tmp_keys.data() + u_mid + 1 + u_right_size,
                  right->keys.data());
        std::copy(tmp_children.data() + u_mid + 1,
                  tmp_children.data() + u_mid + 1 + u_right_size + 1,
                  right->children.data());

        std::copy(tmp_keys.data(), tmp_keys.data() + u_mid, left->keys.data());
        std::copy(tmp_children.data(), tmp_children.data() + u_mid + 1, left->children.data());

        left->size = mid;
        return tmp_keys[u_mid];
    }

    static auto merge_into_left(page_ptr_t left_p, page_ptr_t right_p, Key& parent_sep) noexcept
        -> void {
        auto l{as_node(left_p)};
        auto r{as_node(right_p)};

        const auto l_size{l->get_size()};
        const auto r_size{r->get_size()};

        l->keys[l_size] = parent_sep;
        l->size += 1;

        std::copy_n(r->keys.data(), r_size, l->keys.data() + l->size);
        std::copy_n(r->children.data(), r_size + 1, l->children.data() + l->size);
        l->size += r->size;
    }

    static auto borrow_from_left(page_ptr_t left_p, page_ptr_t node_p, Key& parent_sep) noexcept
        -> void {
        auto l{as_node(left_p)};
        auto n{as_node(node_p)};

        const auto u_size{n->get_size()};

        std::copy_backward(n->keys.data(), n->keys.data() + u_size, n->keys.data() + u_size + 1);
        std::copy_backward(
            n->children.data(), n->children.data() + u_size + 1, n->children.data() + u_size + 2);

        const auto l_size{l->get_size()};
        n->keys[0]     = parent_sep;
        n->children[0] = l->children[l_size];
        n->size += 1;

        parent_sep = l->keys[l_size - 1];
        l->size -= 1;
    }

    static auto borrow_from_right(page_ptr_t right_p, page_ptr_t node_p, Key& parent_sep) noexcept
        -> void {
        auto n{as_node(node_p)};
        auto r{as_node(right_p)};

        const auto n_size{n->get_size()};
        n->keys[n_size]         = parent_sep;
        n->children[n_size + 1] = r->children[0];
        n->size += 1;

        parent_sep = r->keys[0];

        const auto r_size{r->get_size()};
        std::move(r->keys.data() + 1, r->keys.data() + r_size, r->keys.data());
        std::move(r->children.data() + 1, r->children.data() + r_size + 1, r->children.data());
        r->size -= 1;
    }
};

} // namespace cairn::storage::detail
