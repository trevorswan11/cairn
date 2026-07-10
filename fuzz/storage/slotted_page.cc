#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gsl/span>
#include <gtest/gtest.h>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests::fuzz {

using namespace cairn::storage;
using namespace fuzztest;

struct InsertOp {
    std::string data;
};

struct UpdateOp {
    u32         index;
    std::string data;
};

struct RemoveOp {
    u32 index;
};

struct CompactOp {};

using SlottedPageOp = stdx::variant<InsertOp, UpdateOp, RemoveOp, CompactOp>;

void FuzzSlottedPage(const std::vector<SlottedPageOp>& operations) {
    page         p;
    slotted_page sp{p};
    sp.refresh_page();

    std::map<i32, std::string> oracle;
    std::vector<slot_id_t>     active_slots;

    for (const auto& op : operations) {
        op.visit(
            [&](InsertOp iop) {
                gsl::span span{helpers::span_from_string(iop.data)};
                if (auto res{sp.insert(span)}) {
                    slot_id_t slot_id{*res};
                    i32       slot_val{std::to_underlying(slot_id)};
                    if (oracle.contains(slot_val)) {
                        oracle[slot_val] = iop.data;
                    } else {
                        oracle.emplace(slot_val, iop.data);
                        active_slots.emplace_back(slot_id);
                    }
                } else {
                    EXPECT_TRUE(UNWRAP_ERR(res) == error_t::STORAGE_PAGE_FULL);
                }
            },
            [&](UpdateOp uop) {
                if (active_slots.empty()) { return; }
                u32       idx{uop.index % static_cast<u32>(active_slots.size())};
                slot_id_t slot_id{active_slots[idx]};
                i32       slot_val{std::to_underlying(slot_id)};

                gsl::span span{helpers::span_from_string(uop.data)};
                if (auto res{sp.update(slot_id, span)}) {
                    oracle[slot_val] = uop.data;
                } else {
                    EXPECT_TRUE(UNWRAP_ERR(res) == error_t::STORAGE_PAGE_FULL);
                }
            },
            [&](RemoveOp rop) {
                if (active_slots.empty()) { return; }
                u32       idx      = rop.index % active_slots.size();
                slot_id_t slot_id  = active_slots[idx];
                i32       slot_val = std::to_underlying(slot_id);

                EXPECT_TRUE(sp.remove(slot_id));
                oracle.erase(slot_val);
                active_slots.erase(active_slots.begin() + idx);
            },
            [&](CompactOp) { sp.compact(); });

        // Post-operation verification
        EXPECT_GE(sp.slot_count(), static_cast<i32>(oracle.size()));

        for (const auto& [slot_val, expected_str] : oracle) {
            slot_id_t  slot_id{slot_val};
            auto       span{UNWRAP(sp.get(slot_id))};
            const auto actual_str{helpers::string_from_span(span)};
            EXPECT_EQ(actual_str, expected_str);
        }
    }
}

FUZZ_TEST(SlottedPageFuzz, FuzzSlottedPage)
    .WithDomains(VectorOf(OneOf(Map([](std::string d) -> SlottedPageOp { return InsertOp{d}; },
                                    Arbitrary<std::string>()),
                                Map([](u32         idx,
                                       std::string d) -> SlottedPageOp { return UpdateOp{idx, d}; },
                                    Arbitrary<u32>(),
                                    Arbitrary<std::string>()),
                                Map([](u32 idx) -> SlottedPageOp { return RemoveOp{idx}; },
                                    Arbitrary<u32>()),
                                Just(SlottedPageOp{CompactOp{}})))
                     .WithMaxSize(500));

} // namespace cairn::tests::fuzz
