#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <ranges>
#include <type_traits>
#include <utility>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/fixed/vector.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/bplus/default_impl.hh"
#include "storage/bplus/traits.hh"
#include "storage/buffer_pool.hh"
#include "storage/error.hh"
#include "storage/page.hh"

namespace cairn::storage::bplus {

// A concurrent B+tree index over the buffer pool
//
// https://cs186berkeley.net/notes/note4/
template <BPlusNodePayload Key,
          BPlusNodePayload Value,
          usize            PoolSize,
          typename Compare                               = std::less<Key>,
          BPlusLeafTrait<Key, Value>       LeafTrait     = default_leaf_trait<Key, Value>,
          BPlusInternalTrait<Key, Compare> InternalTrait = default_internal_trait<Key>>
class tree_t {
    static constexpr usize TREE_HEIGHT_UPPER_BOUND{64};

  public:
    static constexpr usize LEAF_SLOTS{LeafTrait::SLOTS};
    static constexpr usize INTERNAL_SLOTS{InternalTrait::SLOTS};
    using pool_t        = buffer_pool<PoolSize>;
    using write_guard_t = typename pool_t::write_guard_t;
    using read_guard_t  = typename pool_t::read_guard_t;
    static constexpr auto pool_size{pool_t::pool_size};

    using key_type   = Key;
    using value_type = Value;

  public:
    tree_t(pool_t& pool, page_id_t meta_page) noexcept : pool_{pool}, meta_page_{meta_page} {}

    // Creates a fresh, empty, index and allocates its meta page
    [[nodiscard]] static auto create(pool_t& pool) -> result<tree_t> {
        auto [id, guard]{TRY(pool.new_write())};
        guard.template as<meta_node>()->root.reset();
        guard.mark_dirty();
        return tree_t{pool, id};
    }

    [[nodiscard]] auto meta_page() const noexcept -> page_id_t { return meta_page_; }
    [[nodiscard]] auto empty() -> result<bool> {
        read_guard_t guard{TRY(pool_->fetch_read(meta_page_))};
        return !guard.template as<meta_node>()->root.has_value();
    }

    // Returns the value bound to the key via read-latch crabbing
    [[nodiscard]] auto get(const Key& key) -> result<Value> {
        read_guard_t meta_guard{TRY(pool_->fetch_read(meta_page_))};
        const auto   root{meta_guard.template as<meta_node>()->root};
        if (!root.has_value()) { return stdx::err{error_t::KEY_NOT_FOUND}; }

        read_guard_t guard{TRY(pool_->fetch_read(*root))};
        meta_guard.drop();

        // There should only ever be two locks held during crabbing
        while (kind_of(guard) == node_kind::INTERNAL) {
            read_guard_t child_guard{TRY(pool_->fetch_read(route(guard, key)))};
            guard = std::move(child_guard);
        }

        const i32 idx{leaf_lower_bound(guard, key)};
        if (idx < LeafTrait::size(guard.get()) &&
            equal_t{comp_}(LeafTrait::get_key(guard.get(), idx), key)) {
            return LeafTrait::get_value(guard.get(), idx);
        }
        return stdx::err{error_t::KEY_NOT_FOUND};
    }

    [[nodiscard]] auto contains(const Key& key) -> result<bool> {
        auto found{get(key)};
        if (found) { return true; }
        if (found.error() == error_t::KEY_NOT_FOUND) { return false; }
        return stdx::err{found.error()};
    }

    // Visits all kv pairs in range in ascending order, returning the number of visited entries
    //
    // The `visitor` may return boolean types and return false to stop the scan early. All
    // other return types are allowed but are always ignored.
    template <typename Fn>
    [[nodiscard]] auto
    range_scan(const Key& low, const Key& high, Fn&& visitor, bool inclusive = true)
        -> result<usize> {
        read_guard_t meta_guard{TRY(pool_->fetch_read(meta_page_))};
        const auto   root{meta_guard.template as<meta_node>()->root};
        usize        count{0};
        if (!root.has_value()) { return count; }

        read_guard_t guard{TRY(pool_->fetch_read(*root))};
        meta_guard.drop();

        // Crab down to the leaf with the lower bound
        while (kind_of(guard) == node_kind::INTERNAL) {
            read_guard_t child_guard{TRY(pool_->fetch_read(route(guard, low)))};
            guard = std::move(child_guard);
        }

        i32 idx{leaf_lower_bound(guard, low)};
        while (true) {
            while (idx < LeafTrait::size(guard.get())) {
                const auto k{LeafTrait::get_key(guard.get(), idx)};
                const auto v{LeafTrait::get_value(guard.get(), idx)};
                if (inclusive) {
                    if (greater_t{comp_}(k, high)) { return count; }
                } else {
                    if (!less_t{comp_}(k, high)) { return count; }
                }
                count += 1;

                // The caller has the option to use a void yielding lambda
                using result_t = std::invoke_result_t<Fn, const Key&, const Value&>;
                if constexpr (std::same_as<result_t, bool>) {
                    if (!visitor(k, v)) { return count; }
                } else if constexpr (std::is_void_v<result_t>) {
                    visitor(k, v);
                } else {
                    DISCARD(visitor(k, v));
                }
                idx += 1;
            }

            const auto next{LeafTrait::next(guard.get())};
            if (!next.has_value()) { return count; }

            // Release the current leaf before latching sibling to prevent deadlock
            guard.drop();
            guard = TRY(pool_->fetch_read(*next));
            idx   = 0;
        }
    }

    // Inserts a KV pair only if it does not already exist in the tree
    [[nodiscard]] auto emplace(const Key& key, const Value& value) -> result<void> {
        write_guard_t meta_guard{TRY(fetch_meta_write())};
        auto          meta{meta_guard.template as<meta_node>()};

        if (!meta->root.has_value()) {
            auto [root_pid, leaf_guard]{TRY(pool_->new_write())};

            LeafTrait::init(leaf_guard.get());
            LeafTrait::insert_at(leaf_guard.get(), 0, key, value);
            leaf_guard.mark_dirty();

            meta->root.emplace(root_pid);
            meta_guard.mark_dirty();
            return {};
        }

        // Descend with write crabbing a just hold on to potential splitters
        path_stack                   path;
        stdx::option<write_guard_t&> meta_guard_opt{meta_guard};
        page_id_t                    cur{*meta->root};
        {
            write_guard_t root_guard{TRY(pool_->fetch_write(cur))};
            if (can_emplace(root_guard)) {
                meta_guard_opt->drop();
                meta_guard_opt.reset();
            }
            path.emplace_back(std::move(root_guard));
        }

        while (kind_of(path.back()) == node_kind::INTERNAL) {
            const page_id_t child{route(path.back(), key)};
            write_guard_t   child_guard{TRY(pool_->fetch_write(child))};

            if (can_emplace(child_guard)) {
                // At this point no ancestors can split
                path.clear();
                if (meta_guard_opt) {
                    meta_guard_opt->drop();
                    meta_guard_opt.reset();
                }
            }
            path.emplace_back(std::move(child_guard));
        }

        const i32 idx{leaf_lower_bound(path.back(), key)};
        if (idx < LeafTrait::size(path.back().get()) &&
            equal_t{comp_}(LeafTrait::get_key(path.back().get(), idx), key)) {
            return stdx::err{error_t::DUPLICATE_KEY};
        }

        if (LeafTrait::can_insert(path.back().get())) {
            LeafTrait::insert_at(path.back().get(), idx, key, value);
            path.back().mark_dirty();
            return {};
        }

        // Otherwise the leaf must be full and needs to be split
        auto [up_pid, up_guard]{TRY(pool_->new_write())};
        Key up_key = LeafTrait::split(path.back().get(), up_guard.get(), up_pid, idx, key, value);
        path.back().mark_dirty();
        up_guard.mark_dirty();

        return propagate_split(meta_guard_opt, path, up_key, up_pid);
    }

    // Removes 'key' and returns KEY_NOT_FOUND if absent
    [[nodiscard]] auto remove(const Key& key) -> result<void> {
        write_guard_t meta_guard{TRY(fetch_meta_write())};
        auto          meta{meta_guard.template as<meta_node>()};
        if (!meta->root.has_value()) { return stdx::err{error_t::KEY_NOT_FOUND}; }

        path_stack path;
        slot_stack slot; // index of path[i] inside of path[i-1]

        stdx::option<write_guard_t&> meta_guard_opt{meta_guard};
        page_id_t                    cur{*meta->root};
        {
            write_guard_t root_guard{TRY(pool_->fetch_write(cur))};
            const bool    root_is_leaf{kind_of(root_guard) == node_kind::LEAF};
            if (can_remove(root_guard, true, root_is_leaf)) {
                meta_guard_opt->drop();
                meta_guard_opt.reset();
            }
            path.emplace_back(std::move(root_guard));
            slot.emplace_back(0);
        }

        while (kind_of(path.back()) == node_kind::INTERNAL) {
            const i32       child_slot{internal_upper_bound(path.back(), key)};
            const page_id_t child{InternalTrait::get_child(path.back().get(), child_slot)};

            write_guard_t child_guard{TRY(pool_->fetch_write(child))};
            const bool    child_is_leaf{kind_of(child_guard) == node_kind::LEAF};
            if (can_remove(child_guard, false, child_is_leaf)) {
                if (meta_guard_opt) {
                    meta_guard_opt->drop();
                    meta_guard_opt.reset();
                }
                path.clear();
                slot.clear();
            }

            path.emplace_back(std::move(child_guard));
            slot.emplace_back(child_slot);
        }

        // Remove from the leaf
        const i32 idx{leaf_lower_bound(path.back(), key)};
        if (idx >= LeafTrait::size(path.back().get()) ||
            !equal_t{comp_}(LeafTrait::get_key(path.back().get(), idx), key)) {
            return stdx::err{error_t::KEY_NOT_FOUND};
        }

        LeafTrait::remove_at(path.back().get(), idx);
        path.back().mark_dirty();

        // The root leaf may be emptied which would empty the whole tree
        if (path.size() == 1 && meta_guard_opt) {
            if (LeafTrait::size(path.back().get()) == 0) {
                const page_id_t root_pid{path.back().page_id()};
                path.back().drop();
                path.pop_back();

                meta->root.reset();
                meta_guard.mark_dirty();
                TRY(pool_->delete_page(root_pid));
            }
            return {};
        }

        if (!LeafTrait::can_remove(path.back().get())) {
            return rebalance(meta_guard_opt, path, slot);
        }
        return {};
    }

  private:
    using path_stack = stdx::fixed::vector<write_guard_t, TREE_HEIGHT_UPPER_BOUND>;
    using slot_stack = stdx::fixed::vector<i32, TREE_HEIGHT_UPPER_BOUND>;

    struct less_t {
        const Compare& comp_;

        [[nodiscard]] auto operator()(const Key& a, const Key& b) const -> bool {
            return comp_(a, b);
        }
    };

    struct greater_t {
        const Compare& comp_;

        [[nodiscard]] auto operator()(const Key& a, const Key& b) const -> bool {
            return comp_(b, a);
        }
    };

    struct equal_t {
        const Compare& comp_;

        [[nodiscard]] auto operator()(const Key& a, const Key& b) const -> bool {
            return !less_t{comp_}(a, b) && !greater_t{comp_}(a, b);
        }
    };

    struct meta_node {
        stdx::option<page_id_t> root;
    };
    static_assert(sizeof(meta_node) <= DB_PAGE_SIZE, "meta node overflows a page");
    static_assert(stdx::StandardLayout<meta_node> && stdx::TriviallyCopyable<meta_node>);

  private:
    template <typename Guard> [[nodiscard]] static auto kind_of(const Guard& g) -> node_kind {
        return *g.template as<node_kind>();
    }

    // First index in the leaf whose key is >= `key`
    template <stdx::NumericIntegral I = i32, typename Guard>
    [[nodiscard]] auto leaf_lower_bound(const Guard& g, const Key& key) const -> I {
        const i32 size = LeafTrait::size(g.get());
        return static_cast<I>(std::ranges::lower_bound(std::views::iota(0, size),
                                                       key,
                                                       less_t{comp_},
                                                       [&](const i32& idx) {
                                                           return LeafTrait::get_key(g.get(), idx);
                                                       }) -
                              std::views::iota(0, size).begin());
    }

    // First index whose separator is strictly greater than `key`
    template <stdx::NumericIntegral I = i32, typename Guard>
    [[nodiscard]] auto internal_upper_bound(const Guard& g, const Key& key) const -> I {
        const i32 size = InternalTrait::size(g.get());
        return static_cast<I>(std::ranges::upper_bound(std::views::iota(0, size),
                                                       key,
                                                       less_t{comp_},
                                                       [&](const i32& idx) {
                                                           return InternalTrait::get_key(g.get(),
                                                                                         idx);
                                                       }) -
                              std::views::iota(0, size).begin());
    }

    template <typename Guard>
    [[nodiscard]] auto route(const Guard& g, const Key& key) const -> page_id_t {
        return InternalTrait::get_child(g.get(), internal_upper_bound(g, key));
    }

    [[nodiscard]] static auto can_emplace(const write_guard_t& g) noexcept -> bool {
        if (kind_of(g) == node_kind::LEAF) { return LeafTrait::can_insert(g.get()); }
        return InternalTrait::can_insert(g.get());
    }

    [[nodiscard]] static auto
    can_remove(const write_guard_t& g, bool is_root, bool is_leaf) noexcept -> bool {
        const i32 size{is_leaf ? LeafTrait::size(g.get()) : InternalTrait::size(g.get())};
        if (is_root) { return size > 1; }
        return is_leaf ? LeafTrait::can_remove(g.get()) : InternalTrait::can_remove(g.get());
    }

    // Walks a split separator up the retained ancestor path
    [[nodiscard]] auto propagate_split(stdx::option<write_guard_t&> meta_guard,
                                       path_stack&                  path,
                                       Key                          up_key,
                                       page_id_t                    up_pid) -> result<void> {
        // Loop with isize to prevent accidental underflow
        for (isize i{static_cast<isize>(path.size()) - 2}; i >= 0; --i) {
            const auto u_idx{static_cast<usize>(i)};
            if (InternalTrait::can_insert(path[u_idx].get())) {
                i32 key_idx{0};
                while (key_idx < InternalTrait::size(path[u_idx].get()) &&
                       !less_t{comp_}(up_key, InternalTrait::get_key(path[u_idx].get(), key_idx))) {
                    key_idx += 1;
                }

                InternalTrait::insert_at(path[u_idx].get(), key_idx, up_key, up_pid);
                path[u_idx].mark_dirty();
                return {};
            }

            auto [right_pid, right_guard]{TRY(pool_->new_write())};
            up_key = InternalTrait::split(
                path[u_idx].get(), right_guard.get(), up_key, up_pid, less_t{comp_});
            up_pid = right_pid;

            path[u_idx].mark_dirty();
            right_guard.mark_dirty();
        }

        ASSERT(meta_guard, "root split without holding the meta page latch");
        auto meta{meta_guard->template as<meta_node>()};
        auto [new_root_pid, root_guard]{TRY(pool_->new_write())};

        InternalTrait::init_root(root_guard.get(), up_key, path.front().page_id(), up_pid);

        root_guard.mark_dirty();
        meta->root.emplace(new_root_pid);
        meta_guard->mark_dirty();
        return {};
    }

    // Restores the min-occupancy invariant after a leaf underflow. Handles borrowing and merging
    [[nodiscard]] auto rebalance(stdx::option<write_guard_t&> meta_guard,
                                 path_stack&                  path,
                                 slot_stack&                  slot) -> result<void> {
        // Work from the underflowed leaf up toward the root.
        for (isize level{static_cast<isize>(path.size()) - 1}; level >= 1; --level) {
            const auto u_level{static_cast<usize>(level)};
            const bool is_leaf{kind_of(path[u_level]) == node_kind::LEAF};

            if (is_leaf ? LeafTrait::can_remove(path[u_level].get())
                        : InternalTrait::can_remove(path[u_level].get())) {
                return {};
            }

            const i32 child_slot{slot[u_level]};

            // Try borrowing from a sibling
            const auto borrow_sibling = [&](auto child_offset, auto borrow_fn) -> result<bool> {
                write_guard_t sib{TRY(pool_->fetch_write(
                    InternalTrait::get_child(path[u_level - 1].get(), child_offset)))};

                if (is_leaf ? LeafTrait::can_remove(sib.get())
                            : InternalTrait::can_remove(sib.get())) {
                    // Get parent separator
                    i32 parent_sep_idx = child_slot > child_offset ? child_offset : child_slot;
                    Key parent_sep =
                        InternalTrait::get_key(path[u_level - 1].get(), parent_sep_idx);

                    borrow_fn(sib.get(), path[u_level].get(), parent_sep);

                    // The trait method update the parent_sep out-param
                    InternalTrait::set_key(path[u_level - 1].get(), parent_sep_idx, parent_sep);

                    sib.mark_dirty();
                    path[u_level].mark_dirty();
                    path[u_level - 1].mark_dirty();
                    return true;
                }
                return false;
            };

            if (child_slot > 0) {
                if (TRY(borrow_sibling(child_slot - 1,
                                       is_leaf ? LeafTrait::borrow_from_left
                                               : InternalTrait::borrow_from_left))) {
                    return {};
                }
            }
            if (child_slot < InternalTrait::size(path[u_level - 1].get())) {
                if (TRY(borrow_sibling(child_slot + 1,
                                       is_leaf ? LeafTrait::borrow_from_right
                                               : InternalTrait::borrow_from_right))) {
                    return {};
                }
            }

            // Otherwise a merge is needed
            if (child_slot > 0) {
                write_guard_t   left_sib{TRY(pool_->fetch_write(
                    InternalTrait::get_child(path[u_level - 1].get(), child_slot - 1)))};
                const page_id_t freed{path[u_level].page_id()}; // free current node

                Key parent_sep = InternalTrait::get_key(path[u_level - 1].get(), child_slot - 1);

                if (is_leaf) {
                    LeafTrait::merge_into_left(left_sib.get(), path[u_level].get(), parent_sep);
                } else {
                    InternalTrait::merge_into_left(left_sib.get(), path[u_level].get(), parent_sep);
                }

                InternalTrait::remove_at(path[u_level - 1].get(), child_slot - 1);

                left_sib.mark_dirty();
                path[u_level].drop();
                if (auto r{pool_->delete_page(freed)}; !r) { return stdx::err{r.error()}; }
            } else {
                write_guard_t   right_sib{TRY(pool_->fetch_write(
                    InternalTrait::get_child(path[u_level - 1].get(), child_slot + 1)))};
                const page_id_t freed{right_sib.page_id()}; // free right sibling

                Key parent_sep = InternalTrait::get_key(path[u_level - 1].get(), child_slot);

                if (is_leaf) {
                    LeafTrait::merge_into_left(path[u_level].get(), right_sib.get(), parent_sep);
                } else {
                    InternalTrait::merge_into_left(
                        path[u_level].get(), right_sib.get(), parent_sep);
                }

                InternalTrait::remove_at(path[u_level - 1].get(), child_slot);

                path[u_level].mark_dirty();
                right_sib.drop();
                if (auto r{pool_->delete_page(freed)}; !r) { return stdx::err{r.error()}; }
            }

            // The parent lost a key and occupancy needs to be checked on next iteration
            path[u_level - 1].mark_dirty();
        }

        // The root has been reached and may need to collapse
        if (!meta_guard) { return {}; }
        return maybe_collapse_root(*meta_guard, path.front());
    }

    [[nodiscard]] auto maybe_collapse_root(write_guard_t& meta_guard, write_guard_t& root_guard)
        -> result<void> {
        if (kind_of(root_guard) == node_kind::LEAF) { return {}; }
        if (InternalTrait::size(root_guard.get()) > 0) { return {}; }

        const auto old_root{root_guard.page_id()};
        const auto new_root{InternalTrait::get_child(root_guard.get(), 0)};
        root_guard.drop();
        meta_guard.template as<meta_node>()->root.emplace(new_root);
        meta_guard.mark_dirty();
        return pool_->delete_page(old_root);
    }

    [[nodiscard]] auto fetch_meta_write() { return pool_->fetch_write(meta_page_); }

  private:
    stdx::option<pool_t&>         pool_;
    page_id_t                     meta_page_;
    [[no_unique_address]] Compare comp_;
};

} // namespace cairn::storage::bplus
