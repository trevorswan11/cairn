#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "exec/index_scan.hh"
#include "storage/bplus.hh"
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

TEST_CASE("exec::index_scan range scans over secondary index") {
    helpers::tempfile file{"exec_scan_index_test"};
    using txn_tree_t   = txn::iot_tree<i64, 128, 64>;
    using index_tree_t = storage::bplus_tree<i64, i64, 64>;

    auto                        pool{unwrap(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                        primary_base{unwrap(txn_tree_t::tree_t::create(*pool))};
    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t                  primary_tree{primary_base, undo_mgr};
    index_tree_t                secondary_index{unwrap(index_tree_t::create(*pool))};
    txn::manager                tm;
    helpers::tempfile           wal_file{"exec_scan_index_test_wal"};
    wal::log::manager           lm{wal_file.path, 1_MiB};

    const auto      t1{tm.begin_txn()};
    const std::pair vals{100, "record100"};
    REQUIRE(primary_tree.insert_txn(t1, vals.first, helpers::span_from_string(vals.second)));
    REQUIRE(secondary_index.emplace(10, vals.first));

    // Commit T1
    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(t1, lm));

    // T2 starts and does an index scan
    const auto                    t2{tm.begin_txn()};
    const auto                    snap2{unwrap(tm.acquire_snapshot(t2))};
    index_scan<i64, i64, 128, 64> scanner{secondary_index, primary_tree, t2, snap2, tm};

    std::vector<std::pair<i64, std::string>> results;
    CHECK(unwrap(scanner(0, 50, [&](const i64& pk, gsl::span<const std::byte> val) {
              results.emplace_back(pk, helpers::string_from_span(val));
          })) == 1);

    REQUIRE(results.size() == 1);
    CHECK(results[0] == vals);
}

} // namespace cairn::tests
