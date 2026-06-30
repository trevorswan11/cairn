#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <functional>
#include <iterator>
#include <tuple>
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

#include "storage/buffer_pool.hh"
#include "storage/error.hh"
#include "storage/page.hh"

namespace cairn::storage {

namespace detail {

static constexpr usize LEAF_NODE_PREFIX{16};    // kind+pad+size (8) + next (8)
static constexpr usize INTERNAL_NODE_PREFIX{8}; // kind+pad+size
static constexpr usize TREE_HEIGHT_UPPER_BOUND{64};

template <typename Key, typename Compare> struct less {
    const Compare& comp_;

    [[nodiscard]] auto operator()(const Key& a, const Key& b) const -> bool { return comp_(a, b); }
};

template <typename Key, typename Compare> struct greater {
    const Compare& comp_;

    [[nodiscard]] auto operator()(const Key& a, const Key& b) const -> bool { return comp_(b, a); }
};

template <typename Key, typename Compare> struct equal {
    const Compare& comp_;

    [[nodiscard]] auto operator()(const Key& a, const Key& b) const -> bool {
        return !less<Key, Compare>{comp_}(a, b) && !greater<Key, Compare>{comp_}(a, b);
    }
};

} // namespace detail

// A concurrent B+tree index over the buffer pool
//
// https://cs186berkeley.net/notes/note4/
template <stdx::TriviallyCopyable Key,
          stdx::TriviallyCopyable Value,
          usize                   PoolSize,
          typename Compare = std::less<Key>>
class bplus_tree {
  public:
    using pool_t        = buffer_pool<PoolSize>;
    using write_guard_t = typename pool_t::write_guard_t;
    using read_guard_t  = typename pool_t::read_guard_t;
    static constexpr auto pool_size{pool_t::pool_size};

    using key_type   = Key;
    using value_type = Value;

    enum class node_kind : u8 {
        INTERNAL,
        LEAF,
    };

  public:
    static constexpr usize LEAF_SLOTS{(DB_PAGE_SIZE - detail::LEAF_NODE_PREFIX) /
                                      (sizeof(Key) + sizeof(Value))};
    static_assert(LEAF_SLOTS >= 2, "key/value too large for an 8 KiB leaf");

    static constexpr usize INTERNAL_SLOTS{
        (DB_PAGE_SIZE - detail::INTERNAL_NODE_PREFIX - sizeof(page_id_t)) /
        (sizeof(Key) + sizeof(page_id_t))};
    static_assert(INTERNAL_SLOTS >= 2, "key too large for an 8 KiB internal node");

  public:
    bplus_tree(pool_t& pool, page_id_t meta_page) noexcept : pool_{pool}, meta_page_{meta_page} {}

    // Creates a fresh, empty, index and allocates its meta page
    [[nodiscard]] static auto create(pool_t& pool) -> result<bplus_tree> {
        auto [id, guard]{TRY(pool.new_write())};
        guard.template as<meta_node>()->root.reset();
        guard.mark_dirty();
        return bplus_tree{pool, id};
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
            const auto   node{as_internal(guard)};
            read_guard_t child_guard{TRY(pool_->fetch_read(route(node, key)))};
            guard = std::move(child_guard);
        }

        const auto leaf{as_leaf(guard)};
        const auto idx{leaf_lower_bound<usize>(leaf, key)};
        if (idx < leaf->size_bytes() && equal_t{comp_}(leaf->keys[idx], key)) {
            return leaf->values[idx];
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
            const auto   node{guard.template as<internal_node>()};
            read_guard_t child_guard{TRY(pool_->fetch_read(route(node, low)))};
            guard = std::move(child_guard);
        }

        auto idx{leaf_lower_bound<usize>(guard.template as<leaf_node>(), low)};
        while (true) {
            const auto leaf{guard.template as<leaf_node>()};
            while (idx < leaf->size_bytes()) {
                if (inclusive) {
                    if (greater_t{comp_}(leaf->keys[idx], high)) { return count; }
                } else {
                    if (!less_t{comp_}(leaf->keys[idx], high)) { return count; }
                }
                count += 1;

                // The caller has the option to use a void yielding lambda
                using result_t = std::invoke_result_t<Fn, const Key&, const Value&>;
                if constexpr (std::same_as<result_t, bool>) {
                    if (!visitor(leaf->keys[idx], leaf->values[idx])) { return count; }
                } else if constexpr (std::is_void_v<result_t>) {
                    visitor(leaf->keys[idx], leaf->values[idx]);
                } else {
                    DISCARD(visitor(leaf->keys[idx], leaf->values[idx]));
                }
                idx += 1;
            }

            const auto next{leaf->next};
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
            auto leaf{leaf_guard.template as<leaf_node>()};

            leaf->type = node_kind::LEAF;
            leaf->size = 0;
            leaf->next.reset();

            leaf_emplace_at(leaf, 0, key, value);
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
            if (can_emplace(&root_guard)) {
                meta_guard_opt->drop();
                meta_guard_opt.reset();
            }
            path.emplace_back(std::move(root_guard));
        }

        while (kind_of(path.back()) == node_kind::INTERNAL) {
            const auto      node{as_internal(path.back())};
            const page_id_t child{route(node, key)};
            write_guard_t   child_guard{TRY(pool_->fetch_write(child))};

            if (can_emplace(&child_guard)) {
                // At this point no ancestors can split
                path.clear();
                if (meta_guard_opt) {
                    meta_guard_opt->drop();
                    meta_guard_opt.reset();
                }
            }
            path.emplace_back(std::move(child_guard));
        }

        auto      leaf{as_leaf(path.back())};
        const i32 idx{leaf_lower_bound(leaf, key)};
        if (idx < leaf->size && equal_t{comp_}(leaf->keys[static_cast<usize>(idx)], key)) {
            return stdx::err{error_t::DUPLICATE_KEY};
        }

        if (leaf->size < LEAF_CAP) {
            leaf_emplace_at(leaf, idx, key, value);
            path.back().mark_dirty();
            return {};
        }

        // Otherwise the leaf myst be full and needs to be split
        const auto [up_pid, up_key]{TRY(split_leaf(&path.back(), idx, key, value))};
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
            if (can_remove(&root_guard, true, root_is_leaf)) {
                meta_guard_opt->drop();
                meta_guard_opt.reset();
            }
            path.emplace_back(std::move(root_guard));
            slot.emplace_back(0);
        }

        while (kind_of(path.back()) == node_kind::INTERNAL) {
            const auto      node{as_internal(path.back())};
            const i32       child_slot{internal_upper_bound(node, key)};
            const page_id_t child{node->children[static_cast<usize>(child_slot)]};

            write_guard_t child_guard{TRY(pool_->fetch_write(child))};
            const bool    child_is_leaf{kind_of(child_guard) == node_kind::LEAF};
            if (can_remove(&child_guard, false, child_is_leaf)) {
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
        auto      leaf{as_leaf(path.back())};
        const i32 idx{leaf_lower_bound(leaf, key)};
        if (idx >= leaf->size || !equal_t{comp_}(leaf->keys[static_cast<usize>(idx)], key)) {
            return stdx::err{error_t::KEY_NOT_FOUND};
        }
        leaf_remove_at(leaf, idx);
        path.back().mark_dirty();

        // The root leaf may be emptied which would empty the whole tree
        if (path.size() == 1 && meta_guard_opt) {
            if (leaf->size == 0) {
                const page_id_t root_pid{path.back().page_id()};
                path.back().drop();
                path.pop_back();

                meta->root.reset();
                meta_guard.mark_dirty();
                TRY(pool_->delete_page(root_pid));
            }
            return {};
        }

        if (leaf->size >= LEAF_MIN) { return {}; }
        return rebalance(meta_guard_opt, path, slot);
    }

  private:
    using path_stack = stdx::fixed::vector<write_guard_t, detail::TREE_HEIGHT_UPPER_BOUND>;
    using slot_stack = stdx::fixed::vector<i32, detail::TREE_HEIGHT_UPPER_BOUND>;

    using less_t    = detail::less<Key, Compare>;
    using equal_t   = detail::equal<Key, Compare>;
    using greater_t = detail::greater<Key, Compare>;

    struct meta_node {
        stdx::option<page_id_t> root;
    };
    static_assert(sizeof(meta_node) <= DB_PAGE_SIZE, "meta node overflows a page");
    static_assert(stdx::StandardLayout<meta_node> && stdx::TriviallyCopyable<meta_node>);

    struct leaf_node {
        node_kind                     type;
        std::array<u8, 3>             pad_;
        i32                           size;
        stdx::option<page_id_t>       next;
        std::array<Key, LEAF_SLOTS>   keys;
        std::array<Value, LEAF_SLOTS> values;

        [[nodiscard]] auto size_bytes() const noexcept -> usize { return static_cast<usize>(size); }
    };
    static_assert(sizeof(leaf_node) <= DB_PAGE_SIZE, "leaf node overflows a page");
    static_assert(stdx::StandardLayout<leaf_node> && stdx::TriviallyCopyable<leaf_node>);

    struct internal_node {
        node_kind                                 type;
        std::array<u8, 3>                         pad_;
        i32                                       size;
        std::array<Key, INTERNAL_SLOTS>           keys;
        std::array<page_id_t, INTERNAL_SLOTS + 1> children;

        [[nodiscard]] auto size_bytes() const noexcept -> usize { return static_cast<usize>(size); }
    };
    static_assert(sizeof(internal_node) <= DB_PAGE_SIZE, "internal node overflows a page");
    static_assert(stdx::StandardLayout<internal_node> && stdx::TriviallyCopyable<internal_node>);

  private:
    static constexpr i32 LEAF_CAP{static_cast<i32>(LEAF_SLOTS)};
    static constexpr i32 LEAF_MIN{LEAF_CAP / 2};
    static constexpr i32 INTERNAL_CAP{static_cast<i32>(INTERNAL_SLOTS)};
    static constexpr i32 INTERNAL_MIN{INTERNAL_CAP / 2};

  private:
    template <typename Guard> [[nodiscard]] static auto kind_of(const Guard& g) -> node_kind {
        return *g.template as<node_kind>();
    }

    [[nodiscard]] static auto as_leaf(write_guard_t& g) -> gsl::not_null<leaf_node*> {
        auto n{g.template as<leaf_node>()};
        ASSERT(n->type == node_kind::LEAF, "expected a leaf page");
        return n;
    }

    [[nodiscard]] static auto as_leaf(const read_guard_t& g) -> gsl::not_null<const leaf_node*> {
        auto n{g.template as<leaf_node>()};
        ASSERT(n->type == node_kind::LEAF, "expected a leaf page");
        return n;
    }

    [[nodiscard]] static auto as_internal(write_guard_t& g) -> gsl::not_null<internal_node*> {
        auto n{g.template as<internal_node>()};
        ASSERT(n->type == node_kind::INTERNAL, "expected an internal page");
        return n;
    }

    [[nodiscard]] static auto as_internal(const read_guard_t& g)
        -> gsl::not_null<const internal_node*> {
        auto n{g.template as<internal_node>()};
        ASSERT(n->type == node_kind::INTERNAL, "expected an internal page");
        return n;
    }

    // First index in the leaf whose key is >= `key`
    template <stdx::NumericIntegral I = i32>
    [[nodiscard]] auto leaf_lower_bound(gsl::not_null<const leaf_node*> node, const Key& key) const
        -> I {
        gsl::span active_keys{node->keys.data(), node->size_bytes()};
        auto      it{std::ranges::lower_bound(active_keys, key, less_t{comp_})};
        return static_cast<I>(std::distance(active_keys.begin(), it));
    }

    // First index whose separator is strictly greater than `key`
    template <stdx::NumericIntegral I = i32>
    [[nodiscard]] auto internal_upper_bound(gsl::not_null<const internal_node*> node,
                                            const Key&                          key) const -> I {
        gsl::span active_keys{node->keys.data(), node->size_bytes()};
        auto      it{std::ranges::upper_bound(active_keys, key, less_t{comp_})};
        return static_cast<I>(std::distance(active_keys.begin(), it));
    }

    [[nodiscard]] auto route(gsl::not_null<const internal_node*> node, const Key& key) const
        -> page_id_t {
        return node->children[static_cast<usize>(internal_upper_bound(node, key))];
    }

    [[nodiscard]] static auto can_emplace(gsl::not_null<const write_guard_t*> g) noexcept -> bool {
        if (kind_of(*g) == node_kind::LEAF) { return g->template as<leaf_node>()->size < LEAF_CAP; }
        return g->template as<internal_node>()->size < INTERNAL_CAP;
    }

    [[nodiscard]] static auto
    can_remove(gsl::not_null<const write_guard_t*> g, bool is_root, bool is_leaf) noexcept -> bool {
        const i32 size{is_leaf ? g->template as<leaf_node>()->size
                               : g->template as<internal_node>()->size};

        // The root is never allowed to merge and instead just collapses
        if (is_root) { return size > 1; }
        return size > (is_leaf ? LEAF_MIN : INTERNAL_MIN);
    }

    static auto
    leaf_emplace_at(gsl::not_null<leaf_node*> node, i32 idx, const Key& key, const Value& value)
        -> void {
        ASSERT(idx >= 0, "cannot use negative value as a safe insert index");
        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{node->size_bytes()};

        if (u_idx < u_size) {
            std::copy_backward(&node->keys[u_idx], &node->keys[u_size], &node->keys[u_size + 1]);
            std::copy_backward(
                &node->values[u_idx], &node->values[u_size], &node->values[u_size + 1]);
        }

        node->keys[u_idx]   = key;
        node->values[u_idx] = value;
        node->size += 1;
    }

    static auto leaf_remove_at(gsl::not_null<leaf_node*> leaf, i32 idx) -> void {
        std::move(
            leaf->keys.data() + idx + 1, leaf->keys.data() + leaf->size, leaf->keys.data() + idx);
        std::move(leaf->values.data() + idx + 1,
                  leaf->values.data() + leaf->size,
                  leaf->values.data() + idx);
        leaf->size -= 1;
    }

    static auto internal_emplace_at(gsl::not_null<internal_node*> node,
                                    i32                           key_idx,
                                    const Key&                    key,
                                    page_id_t                     right_child) -> void {
        ASSERT(key_idx >= 0, "cannot use negative value as a safe insert index");
        const auto u_idx{static_cast<usize>(key_idx)};
        const auto u_size{node->size_bytes()};

        // The child pointers are always offset by 1
        if (u_idx < u_size) {
            std::copy_backward(&node->keys[u_idx], &node->keys[u_size], &node->keys[u_size + 1]);
            std::copy_backward(&node->children[u_idx + 1],
                               &node->children[u_size + 1],
                               &node->children[u_size + 2]);
        }

        node->keys[u_idx]         = key;
        node->children[u_idx + 1] = right_child;
        node->size += 1;
    }

    // Splits a full leaf and inserts (key, value) at its logical position
    [[nodiscard]] auto split_leaf(gsl::not_null<write_guard_t*> node_guard,
                                  i32                           idx,
                                  const Key&                    key,
                                  const Value& value) -> result<std::pair<page_id_t, Key>> {
        auto       left{node_guard->template as<leaf_node>()};
        const auto u_idx{static_cast<usize>(idx)};
        const auto u_size{left->size_bytes()};

        // Materialize all leaf slots with new inserted
        std::array<Key, LEAF_SLOTS + 1>   tmp_keys;
        std::array<Value, LEAF_SLOTS + 1> tmp_vals;

        std::copy(left->keys.data(), &left->keys[u_idx], tmp_keys.data());
        std::copy(left->values.data(), &left->values[u_idx], tmp_vals.data());

        tmp_keys[u_idx] = key;
        tmp_vals[u_idx] = value;

        if (u_idx < u_size) {
            std::copy(&left->keys[u_idx], &left->keys[u_size], &tmp_keys[u_idx + 1]);
            std::copy(&left->values[u_idx], &left->values[u_size], &tmp_vals[u_idx + 1]);
        }

        static constexpr i32 total{LEAF_CAP + 1};
        static constexpr i32 left_count{(total + 1) / 2};

        auto [right_pid, right_guard]{TRY(pool_->new_write())};
        auto right{right_guard.template as<leaf_node>()};
        right->type = node_kind::LEAF;
        right->size = total - left_count;
        right->next = left->next;

        const auto u_left_count{left_count};
        const auto u_right_size{right->size_bytes()};

        // Copy the upper half to the new right sibling
        std::copy(
            &tmp_keys[u_left_count], &tmp_keys[u_left_count + u_right_size], right->keys.data());
        std::copy(
            &tmp_vals[u_left_count], &tmp_vals[u_left_count + u_right_size], right->values.data());

        // Overwrite the left node with the lower half
        std::copy(tmp_keys.data(), &tmp_keys[u_left_count], left->keys.data());
        std::copy(tmp_vals.data(), &tmp_vals[u_left_count], left->values.data());
        left->size = left_count;
        left->next = right_pid;

        node_guard->mark_dirty();
        right_guard.mark_dirty();
        return std::make_pair(right_pid, right->keys[0]);
    }

    // Inserts (sep_key, right_child) into a full internal node and splits it
    [[nodiscard]] auto split_internal(gsl::not_null<write_guard_t*> node_guard,
                                      const Key&                    sep_key,
                                      page_id_t right_child) -> result<std::pair<page_id_t, Key>> {
        auto left{node_guard->template as<internal_node>()};

        std::array<Key, INTERNAL_SLOTS + 1>       tmp_keys;
        std::array<page_id_t, INTERNAL_SLOTS + 2> tmp_children;

        // Find the correct insert position before copying into temp buffers
        const gsl::span active_keys{left->keys.data(), left->size_bytes()};
        auto            it = std::ranges::lower_bound(active_keys, sep_key, less_t{comp_});
        const auto      u_pos{static_cast<usize>(std::distance(active_keys.begin(), it))};
        const auto      u_size{left->size_bytes()};

        std::copy(left->keys.data(), &left->keys[u_pos], tmp_keys.data());
        std::copy(left->children.data(), &left->children[u_pos + 1], tmp_children.data());

        tmp_keys[u_pos]         = sep_key;
        tmp_children[u_pos + 1] = right_child;

        if (u_pos < u_size) {
            std::copy(&left->keys[u_pos], &left->keys[u_size], &tmp_keys[u_pos + 1]);
            std::copy(
                &left->children[u_pos + 1], &left->children[u_size + 1], &tmp_children[u_pos + 2]);
        }

        static constexpr const i32 total_keys{INTERNAL_CAP + 1};
        static constexpr const i32 mid{total_keys / 2}; // this key moves up, not into either half

        auto [right_pid, right_guard]{TRY(pool_->new_write())};
        auto right{right_guard.template as<internal_node>()};
        right->type = node_kind::INTERNAL;
        right->size = total_keys - mid - 1;

        const auto u_mid{static_cast<usize>(mid)};
        const auto u_right_size{right->size_bytes()};

        // Copy the upper half to the right node
        std::copy(&tmp_keys[u_mid + 1], &tmp_keys[u_mid + 1 + u_right_size], right->keys.data());
        std::copy(&tmp_children[u_mid + 1],
                  &tmp_children[u_mid + 1 + u_right_size + 1],
                  right->children.data());

        // Overwrite the left node with the lower half
        std::copy(tmp_keys.data(), &tmp_keys[u_mid], left->keys.data());
        std::copy(tmp_children.data(), &tmp_children[u_mid + 1], left->children.data());

        left->size = mid;
        node_guard->mark_dirty();
        right_guard.mark_dirty();
        return std::make_pair(right_pid, tmp_keys[u_mid]);
    }

    // Walks a split separator up the retained ancestor path
    [[nodiscard]] auto propagate_split(stdx::option<write_guard_t&> meta_guard,
                                       path_stack&                  path,
                                       Key                          up_key,
                                       page_id_t                    up_pid) -> result<void> {
        // Loop with isize to prevent accidental underflow
        for (isize i{static_cast<isize>(path.size()) - 2}; i >= 0; --i) {
            const auto u_idx{static_cast<usize>(i)};
            auto       parent{path[u_idx].template as<internal_node>()};
            if (parent->size < INTERNAL_CAP) {
                i32 key_idx{0};
                while (key_idx < parent->size &&
                       !less_t{comp_}(up_key, parent->keys[static_cast<usize>(key_idx)])) {
                    key_idx += 1;
                }

                internal_emplace_at(parent, key_idx, up_key, up_pid);
                path[u_idx].mark_dirty();
                return {};
            }

            std::tie(up_pid, up_key) = TRY(split_internal(&path[u_idx], up_key, up_pid));
        }

        ASSERT(meta_guard, "root split without holding the meta page latch");
        auto meta{meta_guard->template as<meta_node>()};
        auto [new_root_pid, root_guard]{TRY(pool_->new_write())};

        auto root{root_guard.template as<internal_node>()};
        root->type        = node_kind::INTERNAL;
        root->size        = 1;
        root->keys[0]     = up_key;
        root->children[0] = path.front().page_id();
        root->children[1] = up_pid;

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
            const i32  min_keep{is_leaf ? LEAF_MIN : INTERNAL_MIN};
            const i32  occ{is_leaf ? path[u_level].template as<leaf_node>()->size
                                   : path[u_level].template as<internal_node>()->size};
            if (occ >= min_keep) { return {}; }

            auto      parent{path[u_level - 1].template as<internal_node>()};
            const i32 child_slot{slot[u_level]};

            // Try borrowing from a sibling
            const auto borrow_sibling = [&](auto child_offset, auto borrow_fn) -> result<bool> {
                write_guard_t sib{
                    TRY(pool_->fetch_write(parent->children[static_cast<usize>(child_offset)]))};
                const i32 sib_size{is_leaf ? sib.template as<leaf_node>()->size
                                           : sib.template as<internal_node>()->size};

                if (sib_size > min_keep) {
                    borrow_fn(&sib, &path[u_level], parent, child_slot, is_leaf);
                    path[u_level - 1].mark_dirty();
                    return true;
                }
                return false;
            };

            if (child_slot > 0) {
                if (TRY(borrow_sibling(child_slot - 1, borrow_from_left))) { return {}; }
            }
            if (child_slot < parent->size) {
                if (TRY(borrow_sibling(child_slot + 1, borrow_from_right))) { return {}; }
            }

            // Otherwise a merge is needed
            if (child_slot > 0) {
                write_guard_t left_sib{
                    TRY(pool_->fetch_write(parent->children[static_cast<usize>(child_slot - 1)]))};
                const page_id_t freed{path[u_level].page_id()}; // free current node
                merge_into_left(&left_sib, &path[u_level], parent, child_slot, is_leaf);
                path[u_level].drop();
                if (auto r{pool_->delete_page(freed)}; !r) { return stdx::err{r.error()}; }
            } else {
                write_guard_t right_sib{
                    TRY(pool_->fetch_write(parent->children[static_cast<usize>(child_slot + 1)]))};
                const page_id_t freed{right_sib.page_id()}; // free right sibling
                merge_into_left(&path[u_level], &right_sib, parent, child_slot + 1, is_leaf);
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

    // Pulls the largest entry of the left sibling across the parent separator.
    static auto borrow_from_left(gsl::not_null<write_guard_t*> left_sib,
                                 gsl::not_null<write_guard_t*> node,
                                 gsl::not_null<internal_node*> parent,
                                 i32                           child_slot,
                                 bool                          is_leaf) -> void {
        const auto u_child_slot{static_cast<usize>(child_slot)};
        if (is_leaf) {
            auto l{left_sib->template as<leaf_node>()};
            auto n{node->template as<leaf_node>()};

            const auto l_size{l->size_bytes()};
            leaf_emplace_at(n, 0, l->keys[l_size - 1], l->values[l_size - 1]);
            l->size -= 1;
            parent->keys[u_child_slot - 1] = n->keys[0];
        } else {
            auto l{left_sib->template as<internal_node>()};
            auto n{node->template as<internal_node>()};

            std::move_backward(
                n->keys.data(), n->keys.data() + n->size, n->keys.data() + n->size + 1);
            std::move_backward(n->children.data(),
                               n->children.data() + n->size + 1,
                               n->children.data() + n->size + 2);

            {
                const auto l_size{l->size_bytes()};
                n->keys[0]     = parent->keys[u_child_slot - 1];
                n->children[0] = l->children[l_size];
                n->size += 1;
                parent->keys[u_child_slot - 1] = l->keys[l_size - 1];
            }
            l->size -= 1;
        }

        left_sib->mark_dirty();
        node->mark_dirty();
    }

    // Pulls the smallest entry of the right sibling across the parent separator.
    static auto borrow_from_right(gsl::not_null<write_guard_t*> right_sib,
                                  gsl::not_null<write_guard_t*> node,
                                  gsl::not_null<internal_node*> parent,
                                  i32                           child_slot,
                                  bool                          is_leaf) -> void {
        const auto u_child_slot{static_cast<usize>(child_slot)};
        if (is_leaf) {
            auto n{node->template as<leaf_node>()};
            auto r{right_sib->template as<leaf_node>()};
            leaf_emplace_at(n, n->size, r->keys[0], r->values[0]);
            leaf_remove_at(r, 0);
            parent->keys[u_child_slot] = r->keys[0];
        } else {
            auto n{node->template as<internal_node>()};
            auto r{right_sib->template as<internal_node>()};

            {
                const auto n_size{n->size_bytes()};
                n->keys[n_size]         = parent->keys[u_child_slot];
                n->children[n_size + 1] = r->children[0];
            }
            n->size += 1;
            parent->keys[u_child_slot] = r->keys[0];

            std::move(r->keys.data() + 1, r->keys.data() + r->size, r->keys.data());
            std::move(r->children.data() + 1, r->children.data() + r->size + 1, r->children.data());
            r->size -= 1;
        }

        node->mark_dirty();
        right_sib->mark_dirty();
    }

    // Folds the right node into the left node and drops the parent separator
    static auto merge_into_left(gsl::not_null<write_guard_t*> left,
                                gsl::not_null<write_guard_t*> right,
                                gsl::not_null<internal_node*> parent,
                                i32                           right_slot,
                                bool                          is_leaf) -> void {
        if (is_leaf) {
            auto l{left->template as<leaf_node>()};
            auto r{right->template as<leaf_node>()};
            std::copy_n(r->keys.data(), r->size, l->keys.data() + l->size);
            std::copy_n(r->values.data(), r->size, l->values.data() + l->size);
            l->size += r->size;
            l->next = r->next;
        } else {
            auto l{left->template as<internal_node>()};
            auto r{right->template as<internal_node>()};

            // Pull the separator down first
            l->keys[l->size_bytes()] = parent->keys[static_cast<usize>(right_slot - 1)];
            l->size += 1;
            std::copy_n(r->keys.data(), r->size, l->keys.data() + l->size);
            std::copy_n(r->children.data(), r->size + 1, l->children.data() + l->size);
            l->size += r->size;
        }

        // Remove the parent separator and the right child pointer from the parent
        if ((parent->size - 1) - (right_slot - 1) > 0) {
            std::move(parent->keys.data() + right_slot,
                      parent->keys.data() + parent->size,
                      parent->keys.data() + (right_slot - 1));
        }

        if (parent->size - right_slot > 0) {
            std::move(parent->children.data() + right_slot + 1,
                      parent->children.data() + parent->size + 1,
                      parent->children.data() + right_slot);
        }

        parent->size -= 1;
        left->mark_dirty();
    }

    [[nodiscard]] auto maybe_collapse_root(write_guard_t& meta_guard, write_guard_t& root_guard)
        -> result<void> {
        if (kind_of(root_guard) == node_kind::LEAF) { return {}; }
        auto root{root_guard.template as<internal_node>()};
        if (root->size > 0) { return {}; }

        const auto old_root{root_guard.page_id()};
        const auto new_root{root->children[0]};
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

} // namespace cairn::storage
