#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "exec/table_scan.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace stdx::size_literals;
using namespace cairn::exec;
using helpers::unwrap;

TEST_CASE("exec::table_scan snapshot isolation and visibility range scans") {
    helpers::tempfile file{"exec_scan_table_test"};
    using txn_tree_t = txn::iot_tree<i64, 128, 64>;
    auto                        pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                        base_tree{unwrap(txn_tree_t::tree_t::create(*pool))};
    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t                  tree{base_tree, undo_mgr};
    txn::manager                tm;
    helpers::tempfile           wal_file{"exec_scan_table_test_wal"};
    wal::log::manager           lm{wal_file.path, 1_MiB};

    std::vector<std::pair<i64, std::string>> results;
    const auto visitor = [&](const i64& k, gsl::span<const std::byte> v) {
        results.emplace_back(k, helpers::string_from_span(v));
    };

    // 1. Transaction 1 starts and inserts two records: key 10, key 20
    const auto      t1{tm.begin_txn()};
    const std::pair record1{10, "val10_v1"};
    REQUIRE(tree.insert_txn(t1, record1.first, helpers::span_from_string(record1.second)));
    const std::pair record2{20, "val20_v1"};
    REQUIRE(tree.insert_txn(t1, record2.first, helpers::span_from_string(record2.second)));

    // 2. Transaction 2 starts. It shouldn't see anything yet
    {
        const auto               t2{tm.begin_txn()};
        const auto               snap{unwrap(tm.acquire_snapshot(t2))};
        table_scan<i64, 128, 64> scanner{tree, t2, snap, tm};

        CHECK(unwrap(scanner.scan(0, 100, visitor)) == 0);
        CHECK(results.empty());
    }

    // 3. Commit Transaction 1
    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(t1, lm));

    // Transaction 3 starts. It should see both records
    {
        results.clear();
        const auto               t3{tm.begin_txn()};
        const auto               snap{unwrap(tm.acquire_snapshot(t3))};
        table_scan<i64, 128, 64> scanner{tree, t3, snap, tm};

        CHECK(unwrap(scanner.scan(0, 100, visitor)) == 2);
        REQUIRE(results.size() == 2);
        CHECK(results[0] == record1);
        CHECK(results[1] == record2);

        REQUIRE(tree.update_txn(t3, 10, helpers::span_from_string("val10_v2")));
    }

    // Transaction 4 starts. It should see "val10_v1" (via undo log traversal) and "val20_v1"
    {
        results.clear();
        const auto               t4{tm.begin_txn()};
        const auto               snap{unwrap(tm.acquire_snapshot(t4))};
        table_scan<i64, 128, 64> scanner{tree, t4, snap, tm};

        CHECK(unwrap(scanner.scan(0, 100, visitor)) == 2);
        REQUIRE(results.size() == 2);
        CHECK(results[0] == record1);
        CHECK(results[1] == record2);
    }
}

} // namespace cairn::tests
