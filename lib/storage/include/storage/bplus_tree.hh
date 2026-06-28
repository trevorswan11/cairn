#pragma once

#include <array>
#include <functional>

#include <stdx/fixed/vector.hh>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
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
    };
    static_assert(sizeof(leaf_node) <= DB_PAGE_SIZE, "leaf node overflows a page");
    static_assert(stdx::StandardLayout<leaf_node> && stdx::TriviallyCopyable<leaf_node>);

    struct internal_node {
        node_kind                                 type;
        i32                                       size;
        std::array<Key, INTERNAL_SLOTS>           keys;
        std::array<page_id_t, INTERNAL_SLOTS + 1> children;
    };
    static_assert(sizeof(internal_node) <= DB_PAGE_SIZE, "internal node overflows a page");
    static_assert(stdx::StandardLayout<internal_node> && stdx::TriviallyCopyable<internal_node>);

  private:
    static constexpr i32 LEAF_CAP{static_cast<i32>(LEAF_SLOTS)};
    static constexpr i32 LEAF_MIN{LEAF_CAP / 2};
    static constexpr i32 INTERNAL_CAP{static_cast<i32>(INTERNAL_SLOTS)};
    static constexpr i32 INTERNAL_MIN{INTERNAL_CAP / 2};

  private:
  private:
    stdx::option<pool_t&> pool_;
    page_id_t             meta_page_;
    Compare               comp_{};
};

} // namespace cairn::storage
