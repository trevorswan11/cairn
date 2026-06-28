#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/fixed/vector.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "storage/error.hh"
#include "storage/page.hh"

namespace cairn::storage {

namespace detail {

static constexpr usize LEAF_NODE_PREFIX{16};    // kind+pad+size (8) + next (8)
static constexpr usize INTERNAL_NODE_PREFIX{8}; // kind+pad+size
static constexpr usize TREE_HEIGHT_UPPER_BOUND{64};

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

    using key_type   = Key;
    using value_type = Value;

    enum class node_kind : u32 {
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

    [[nodiscard]] static constexpr auto size() noexcept -> usize { return PoolSize; }
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
        const auto idx{static_cast<usize>(leaf_lower_bound(leaf, key))};
        if (idx < leaf->size_bytes() && comp_.eq(leaf->keys[idx], key)) {
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

  private:
    using path_stack = stdx::fixed::vector<write_guard_t, detail::TREE_HEIGHT_UPPER_BOUND>;
    using slot_stack = stdx::fixed::vector<i32, detail::TREE_HEIGHT_UPPER_BOUND>;

    struct meta_node {
        stdx::option<page_id_t> root;
    };
    static_assert(sizeof(meta_node) <= DB_PAGE_SIZE, "meta node overflows a page");
    static_assert(stdx::StandardLayout<meta_node> && stdx::TriviallyCopyable<meta_node>);

    struct leaf_node {
        node_kind                     type;
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
        i32                                       size;
        std::array<Key, INTERNAL_SLOTS>           keys;
        std::array<page_id_t, INTERNAL_SLOTS + 1> children;

        [[nodiscard]] auto size_bytes() const noexcept -> usize { return static_cast<usize>(size); }
    };
    static_assert(sizeof(internal_node) <= DB_PAGE_SIZE, "internal node overflows a page");
    static_assert(stdx::StandardLayout<internal_node> && stdx::TriviallyCopyable<internal_node>);

    struct compare_t {
        Compare compare_fn_{};

        [[nodiscard]] auto less(const Key& a, const Key& b) const -> bool {
            return compare_fn_(a, b);
        }

        [[nodiscard]] auto eq(const Key& a, const Key& b) const -> bool {
            return !less(a, b) && !less(b, a);
        }
    };

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
        auto* n{g.template as<leaf_node>()};
        ASSERT(n->type == node_kind::LEAF, "expected a leaf page");
        return n;
    }

    [[nodiscard]] static auto as_leaf(const read_guard_t& g) -> gsl::not_null<const leaf_node*> {
        auto* n{g.template as<leaf_node>()};
        ASSERT(n->type == node_kind::LEAF, "expected a leaf page");
        return n;
    }

    [[nodiscard]] static auto as_internal(write_guard_t& g) -> gsl::not_null<internal_node*> {
        auto* n{g.template as<internal_node>()};
        ASSERT(n->type == node_kind::INTERNAL, "expected an internal page");
        return n;
    }

    [[nodiscard]] static auto as_internal(const read_guard_t& g)
        -> gsl::not_null<const internal_node*> {
        auto* n{g.template as<internal_node>()};
        ASSERT(n->type == node_kind::INTERNAL, "expected an internal page");
        return n;
    }

    // First index in the leaf whose key is >= `key`
    [[nodiscard]] auto leaf_lower_bound(gsl::not_null<const leaf_node*> node, const Key& key) const
        -> i32 {
        gsl::span active_keys{node->keys.data(), node->size_bytes()};
        auto      it{
            std::ranges::lower_bound(active_keys, key, [this](const Key& a, const Key& b) -> bool {
                return comp_.less(a, b);
            })};
        return static_cast<i32>(std::distance(active_keys.begin(), it));
    }

    // First index whose separator is strictly greater than `key`
    [[nodiscard]] auto internal_upper_bound(gsl::not_null<const internal_node*> node,
                                            const Key&                          key) const -> i32 {
        gsl::span active_keys{node->keys.data(), node->size_bytes()};
        auto      it{
            std::ranges::upper_bound(active_keys, key, [this](const Key& a, const Key& b) -> bool {
                return comp_.less(a, b);
            })};
        return static_cast<i32>(std::distance(active_keys.begin(), it));
    }

    [[nodiscard]] auto route(gsl::not_null<const internal_node*> node, const Key& key) const
        -> page_id_t {
        return node->children[static_cast<usize>(internal_upper_bound(node, key))];
    }

    [[nodiscard]] auto fetch_meta_write() { return pool_->fetch_write(meta_page_); }

  private:
    stdx::option<pool_t&> pool_;
    page_id_t             meta_page_;
    compare_t             comp_;
};

} // namespace cairn::storage
