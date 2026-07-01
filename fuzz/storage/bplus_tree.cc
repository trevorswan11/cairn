#include <map>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "storage/bplus_tree.hh"
#include "storage/error.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests::fuzz {

using namespace cairn::storage;
using namespace fuzztest;

struct InsertOp {
    u64 key;
    u64 value;
};

struct RemoveOp {
    u64 key;
};

struct GetOp {
    u64 key;
};

using TreeOp = stdx::variant<InsertOp, RemoveOp, GetOp>;

void FuzzBPlusTreeInvariants(const std::vector<TreeOp>& operations) {
    helpers::tempfile file{"bpt_wide"};
    using tree_t = bplus_tree<u64, u64, 64>;
    auto pool{helpers::unwrap(tree_t::pool_t::open(file.path))};
    auto tree{helpers::unwrap(tree_t::create(*pool))};

    std::map<u64, u64> oracle;
    for (const auto& op : operations) {
        op.visit(
            [&](InsertOp iop) {
                auto res{tree.emplace(iop.key, iop.value)};
                if (oracle.contains(iop.key)) {
                    // Key exists: B+tree must report DUPLICATE_KEY
                    EXPECT_TRUE(helpers::unwrap_err(res) == error_t::DUPLICATE_KEY);
                    return;
                }

                // Key is new: B+tree must succeed
                EXPECT_TRUE(res.has_value());
                if (res) { oracle.emplace(iop.key, iop.value); }
            },
            [&](RemoveOp rop) {
                auto res{tree.remove(rop.key)};
                auto ref_it{oracle.find(rop.key)};

                if (ref_it != oracle.end()) {
                    EXPECT_TRUE(res.has_value());
                    oracle.erase(ref_it);
                } else {
                    EXPECT_FALSE(res.has_value());
                }
            },
            [&](GetOp gop) {
                auto res{tree.get(gop.key)};
                auto ref_it{oracle.find(gop.key)};

                if (ref_it != oracle.end()) {
                    ASSERT_TRUE(res.has_value());
                    EXPECT_EQ(*res, ref_it->second);
                } else {
                    EXPECT_FALSE(res.has_value());
                }
            });
    }

    if (!oracle.empty()) {
        u64 low{oracle.begin()->first};
        u64 high{oracle.rbegin()->first};

        usize scan_count{0};
        EXPECT_EQ(helpers::unwrap(tree.range_scan(low,
                                                  high,
                                                  [&](const u64& k, const u64& v) {
                                                      auto it = oracle.find(k);
                                                      EXPECT_NE(it, oracle.end());
                                                      if (it != oracle.end()) {
                                                          EXPECT_EQ(v, it->second);
                                                      }
                                                      scan_count++;
                                                  })),
                  scan_count);
    }
}

FUZZ_TEST(BPlusTreeFuzz, FuzzBPlusTreeInvariants)
    .WithDomains(VectorOf(OneOf(Map([](u64 k, u64 v) -> TreeOp { return InsertOp{k, v}; },
                                    Arbitrary<u64>(),
                                    Arbitrary<u64>()),
                                Map([](u64 k) -> TreeOp { return RemoveOp{k}; }, Arbitrary<u64>()),
                                Map([](u64 k) -> TreeOp { return GetOp{k}; }, Arbitrary<u64>())))
                     .WithMaxSize(1'000));

} // namespace cairn::tests::fuzz
