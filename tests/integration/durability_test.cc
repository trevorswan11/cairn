#include <cstddef>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/types.hh>

#include "storage/bplus.hh"
#include "storage/page.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

TEST_CASE("Index-organized table durability smoke test") {
    helpers::tempfile file{"iot_durability_smoke"};
    using tree_t = storage::iot_tree<i64, 128, 64>;
    storage::page_id_t meta;

    const std::string_view val1{"persistence test data one"};
    const std::string      val2(120, 'd');
    const std::string_view val3{"short test data"};

    // Open pool & create tree
    {
        auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
        auto tree{helpers::unwrap(tree_t::create(*pool))};
        meta = tree.meta_page();

        REQUIRE(tree.emplace(10, helpers::span_from_string(val1)));
        REQUIRE(tree.emplace(20, helpers::span_from_string(val2)));
        REQUIRE(tree.emplace(30, helpers::span_from_string(val3)));
        REQUIRE(pool->flush());
    }

    // Reopen the data files, simulating a process restart
    {
        auto   pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
        tree_t tree{*pool, meta};

        CHECK(helpers::unwrap(
                  tree.range_scan(10, 30, [&](const i64& k, const gsl::span<const std::byte>& v) {
                      const auto str_v{helpers::string_from_span(v)};
                      if (k == 10) {
                          CHECK(str_v == val1);
                      } else if (k == 20) {
                          CHECK(str_v == val2);
                      } else if (k == 30) {
                          CHECK(str_v == val3);
                      }
                  })) == 3);
    }
}

} // namespace cairn::tests
