#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/types.hh>

#include "storage/bplus.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::storage;

TEST_CASE("iot_tree stores and retrieves variable-length tuples") {
    helpers::tempfile file{"iot_test"};
    using tree_t = iot_tree<i64, 128, 64>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};

    const std::string_view val1 = "hello world";
    const std::string_view val2 =
        "this is a slightly longer tuple string that still fits within 128 bytes";
    const std::string_view val3 = "short";

    auto span1 = gsl::span{reinterpret_cast<const std::byte*>(val1.data()), val1.size()};
    auto span2 = gsl::span{reinterpret_cast<const std::byte*>(val2.data()), val2.size()};
    auto span3 = gsl::span{reinterpret_cast<const std::byte*>(val3.data()), val3.size()};

    helpers::unwrap(tree.emplace(1, span1));
    helpers::unwrap(tree.emplace(2, span2));
    helpers::unwrap(tree.emplace(3, span3));

    // We use range_scan to read values safely while the page is pinned
    CHECK(helpers::unwrap(
              tree.range_scan(1, 3, [&](const i64& k, const gsl::span<const std::byte>& v) {
                  std::string_view str_v{reinterpret_cast<const char*>(v.data()), v.size()};
                  if (k == 1) {
                      CHECK(str_v == val1);
                  } else if (k == 2) {
                      CHECK(str_v == val2);
                  } else if (k == 3) {
                      CHECK(str_v == val3);
                  }
              })) == 3);
}

TEST_CASE("iot_tree handles compaction and removal") {
    helpers::tempfile file{"iot_compaction"};
    using tree_t = iot_tree<i64, 128, 64>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};

    std::array<std::byte, 100> big_val;
    std::fill(big_val.begin(), big_val.end(), std::byte{'A'});
    auto span = gsl::span<const std::byte>{big_val};

    // Insert many elements to trigger splits and compaction
    for (i64 i = 0; i < 100; ++i) { helpers::unwrap(tree.emplace(i, span)); }
    for (i64 i = 0; i < 50; ++i) { helpers::unwrap(tree.remove(i)); }

    // Verify the remaining elements are intact
    CHECK(helpers::unwrap(
              tree.range_scan(50, 99, [&](const i64& k, const gsl::span<const std::byte>& v) {
                  CHECK(k >= 50);
                  CHECK(k < 100);
                  CHECK(v.size() == 100);
                  CHECK(v[0] == std::byte{'A'});
              })) == 50);
}

} // namespace cairn::tests
