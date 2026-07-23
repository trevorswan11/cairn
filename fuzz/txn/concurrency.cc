#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>
#include <stdx/memory.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "support/diagnostic/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/lock/manager.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests::fuzz {

using namespace fuzztest;
using namespace stdx::size_literals;

struct BeginOp {
    u8 txn_idx;
    u8 level;
};

struct ReadOp {
    u8 txn_idx;
    u8 key;
};

struct WriteOp {
    u8  txn_idx;
    u8  key;
    u32 value;
};

struct CommitOp {
    u8 txn_idx;
};

struct AbortOp {
    u8 txn_idx;
};

using ConcurrencyOp = stdx::variant<BeginOp, ReadOp, WriteOp, CommitOp, AbortOp>;

auto FuzzTxnConcurrency(const std::vector<ConcurrencyOp>& operations) -> void {
    helpers::tempfile file{"fuzz_concurrency_db"};
    using txn_tree_t = txn::iot_tree<i64, 128, 64>;
    auto                        pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                        base_tree{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t                  tree{base_tree, undo_mgr};
    txn::manager                tm;
    txn::lock::manager          lm_lock;
    tm.set_lock_manager(lm_lock);
    helpers::tempfile wal_file{"fuzz_concurrency_db_wal"};
    wal::log::manager lm{wal_file.path, 1_MiB};

    struct TxnState {
        txn::id_t                                id{txn::INVALID_TXN_ID};
        txn::isolation_level_t                   level{txn::isolation_level_t::SNAPSHOT};
        bool                                     active{false};
        std::vector<std::pair<i64, std::string>> reads;
        std::vector<i64>                         modified_keys;
    };
    std::array<TxnState, 2> txns;

    for (const auto& op : operations) {
        op.visit(
            [&](BeginOp bop) {
                const auto idx{static_cast<usize>(bop.txn_idx % 2)};
                if (txns[idx].active) {
                    EXPECT_TRUE(tree.rollback_txn(txns[idx].id));
                    EXPECT_TRUE(tm.abort_txn(txns[idx].id, lm));
                    txns[idx].active = false;
                    txns[idx].reads.clear();
                    txns[idx].modified_keys.clear();
                }

                const auto isolation{static_cast<txn::isolation_level_t>(bop.level % 4)};

                txns[idx].id     = tm.begin_txn(isolation);
                txns[idx].level  = isolation;
                txns[idx].active = true;
                txns[idx].reads.clear();
                txns[idx].modified_keys.clear();
            },
            [&](ReadOp rop) {
                const auto idx{static_cast<usize>(rop.txn_idx % 2)};
                if (!txns[idx].active) { return; }

                const auto txn_id{txns[idx].id};
                const auto snap{UNWRAP(tm.acquire_snapshot(txn_id))};

                std::vector<std::byte> buf;
                auto       get_res{UNWRAP(tree.get_txn(txn_id, snap, tm, rop.key, buf))};
                const auto val_str{get_res.transform(helpers::string_from_span).value_or("")};

                // Verify Repeatable Reads behavior
                if (txns[idx].level != txn::isolation_level_t::READ_COMMITTED) {
                    for (const auto& prev_read : txns[idx].reads) {
                        if (prev_read.first == rop.key) { EXPECT_EQ(prev_read.second, val_str); }
                    }
                }
                txns[idx].reads.emplace_back(rop.key, val_str);
            },
            [&](WriteOp wop) {
                const auto idx{static_cast<usize>(wop.txn_idx % 2)};
                if (!txns[idx].active) { return; }

                // Skip write if the other active transaction has modified the same key to avoid
                // write-write conflicts
                const auto other_idx{(idx + 1) % 2};
                if (txns[other_idx].active) {
                    if (std::ranges::find(txns[other_idx].modified_keys,
                                          static_cast<i64>(wop.key)) !=
                        txns[other_idx].modified_keys.end()) {
                        return;
                    }
                }

                const auto txn_id{txns[idx].id};
                const auto val_str{std::to_string(wop.value % 1'000)};
                auto       write_res{
                    tree.update_txn(txn_id, wop.key, helpers::span_from_string(val_str))};
                if (!write_res && write_res.error() == error::STORAGE_KEY_NOT_FOUND) {
                    EXPECT_TRUE(
                        tree.insert_txn(txn_id, wop.key, helpers::span_from_string(val_str)));
                }
                txns[idx].modified_keys.emplace_back(wop.key);
            },
            [&](CommitOp cop) {
                const auto idx{static_cast<usize>(cop.txn_idx % 2)};
                if (!txns[idx].active) { return; }

                const auto txn_id{txns[idx].id};
                EXPECT_TRUE(tm.update_txn_lsn(txn_id, wal::log::seq_num{2}));
                if (auto res{tm.commit_txn(txn_id, lm)}; !res) {
                    EXPECT_TRUE(res.error() == error::TXN_SERIALIZATION_FAILURE ||
                                res.error() == error::TXN_DEADLOCK_DETECTED);
                    EXPECT_TRUE(tree.rollback_txn(txn_id));
                    EXPECT_TRUE(tm.abort_txn(txn_id, lm));
                }
                txns[idx].active = false;
                txns[idx].reads.clear();
                txns[idx].modified_keys.clear();
            },
            [&](AbortOp aop) {
                const auto idx{static_cast<usize>(aop.txn_idx % 2)};
                if (!txns[idx].active) { return; }

                const auto txn_id = txns[idx].id;
                EXPECT_TRUE(tree.rollback_txn(txn_id));
                EXPECT_TRUE(tm.abort_txn(txn_id, lm));
                txns[idx].active = false;
                txns[idx].reads.clear();
                txns[idx].modified_keys.clear();
            });
    }

    for (auto& txn : txns) {
        if (txn.active) {
            EXPECT_TRUE(tree.rollback_txn(txn.id));
            EXPECT_TRUE(tm.abort_txn(txn.id, lm));
        }
    }
}

FUZZ_TEST(ConcurrencyFuzz, FuzzTxnConcurrency)
    .WithDomains(
        VectorOf(OneOf(Map([](u8 idx, u8 lvl) -> ConcurrencyOp { return BeginOp{idx, lvl}; },
                           Arbitrary<u8>(),
                           Arbitrary<u8>()),
                       Map([](u8 idx, u8 key) -> ConcurrencyOp { return ReadOp{idx, key}; },
                           Arbitrary<u8>(),
                           Arbitrary<u8>()),
                       Map([](u8 idx, u8 key, u32 val)
                               -> ConcurrencyOp { return WriteOp{idx, key, val}; },
                           Arbitrary<u8>(),
                           Arbitrary<u8>(),
                           Arbitrary<u32>()),
                       Map([](u8 idx) -> ConcurrencyOp { return CommitOp{idx}; }, Arbitrary<u8>()),
                       Map([](u8 idx) -> ConcurrencyOp { return AbortOp{idx}; }, Arbitrary<u8>())))
            .WithMaxSize(200));

} // namespace cairn::tests::fuzz
