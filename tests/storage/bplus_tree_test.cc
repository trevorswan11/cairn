#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "storage/bplus_tree.hh"
#include "storage/buffer_pool.hh"
#include "storage/error.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::storage;

TEST_CASE("bplus_tree handles an empty tree") {
    helpers::tempfile file{"bpt_empty"};
    auto              pool{helpers::unwrap(buffer_pool<64>::open(file.path))};
    auto              tree{helpers::unwrap(bplus_tree<i64, u64, 64>::create(*pool))};

    CHECK(helpers::unwrap(tree.empty()));
    CHECK(helpers::unwrap_err(tree.get(42)) == error_t::KEY_NOT_FOUND);
}

} // namespace cairn::tests
