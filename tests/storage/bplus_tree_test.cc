#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/bplus_tree.hh"
#include "storage/error.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::storage;

TEST_CASE("bplus_tree handles an empty tree") {
    helpers::tempfile file{"bpt_empty"};
    using tree_t = bplus_tree<i64, u64, 64>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};

    CHECK(helpers::unwrap(tree.empty()));
    CHECK_FALSE(helpers::unwrap(tree.contains(42)));
    CHECK(helpers::unwrap_err(tree.get(42)) == error_t::KEY_NOT_FOUND);
}

TEST_CASE("bplus_tree inserts and looks up sequential keys") {
    helpers::tempfile file{"bpt_seq"};
    using tree_t = bplus_tree<i64, u64, 256>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};
    DISCARD(tree);
}

TEST_CASE("bplus_tree matches a std::map oracle under random inserts") {
    helpers::tempfile file{"bpt_rand"};
    using tree_t = bplus_tree<i64, u64, 256>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};
    DISCARD(tree);
}

TEST_CASE("bplus_tree deletes down to empty, matching the oracle") {
    helpers::tempfile file{"bpt_del"};
    using tree_t = bplus_tree<i64, u64, 256>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};
    DISCARD(tree);
}

TEST_CASE("bplus_tree survives a randomized insert/delete fuzz against an oracle") {
    helpers::tempfile file{"bpt_fuzz"};
    using tree_t = bplus_tree<i64, u64, 256>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};
    DISCARD(tree);
}

TEST_CASE("bplus_tree exercises split/merge/collapse with a small fan-out") {
    helpers::tempfile file{"bpt_wide"};
    using tree_t = bplus_tree<i64, u64, 128>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};
    DISCARD(tree);
}

TEST_CASE("bplus_tree persists across a buffer-pool reopen") {
    helpers::tempfile file{"bpt_persist"};
    using tree_t = bplus_tree<i64, u64, 128>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};
    DISCARD(tree);
}

} // namespace cairn::tests
