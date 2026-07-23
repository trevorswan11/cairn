#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "exec/index_scan.hh"
#include "storage/bplus.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace stdx::size_literals;
using namespace cairn::exec;

TEST_CASE("exec::index_scan range scans over secondary index") {
    helpers::tempfile file{"exec_scan_index_test"};
    using txn_tree_t   = txn::iot_tree<i64, 128, 64>;
    using index_tree_t = storage::bplus_tree<i64, i64, 64>;

    auto                        pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                        primary_base{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t                  primary_tree{primary_base, undo_mgr};
    index_tree_t                secondary_index{UNWRAP(index_tree_t::create(*pool))};
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
    const auto                    snap2{UNWRAP(tm.acquire_snapshot(t2))};
    index_scan<i64, i64, 128, 64> scanner{secondary_index, primary_tree, t2, snap2, tm};

    std::vector<std::pair<i64, std::string>> results;
    CHECK(UNWRAP(scanner(0, 50, [&](const i64& pk, gsl::span<const std::byte> val) {
              results.emplace_back(pk, helpers::string_from_span(val));
          })) == 1);

    REQUIRE(results.size() == 1);
    CHECK(results[0] == vals);
}

TEST_CASE("exec::index_scan_read_set check_conflict detection") {
    helpers::tempfile file{"exec_scan_index_read_set_test"};
    using txn_tree_t   = txn::iot_tree<i64, 128, 64>;
    using index_tree_t = storage::bplus_tree<i64, i64, 64>;

    auto                        pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                        primary_base{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t                  primary_tree{primary_base, undo_mgr};
    index_tree_t                secondary_index{UNWRAP(index_tree_t::create(*pool))};

    const txn::id_t t1{1};
    REQUIRE(primary_tree.insert_txn(t1, 100, helpers::span_from_string("val100")));
    REQUIRE(secondary_index.emplace(10, 100));

    index_scan_read_set<i64, i64, 128, 64> read_set{secondary_index, primary_tree};
    read_set.add_range(0, 50);

    // No conflict when get_commit_ts returns lower timestamp
    CHECK_FALSE(UNWRAP(read_set.check_conflict(
        txn::id_t{2}, txn::timestamp_t{10}, [](txn::id_t tid) -> stdx::option<txn::timestamp_t> {
            if (tid == txn::id_t{1}) { return txn::timestamp_t{5}; }
            return stdx::none;
        })));

    // Conflict detected when committed after reader read_ts
    CHECK(UNWRAP(read_set.check_conflict(
        txn::id_t{2}, txn::timestamp_t{4}, [](txn::id_t tid) -> stdx::option<txn::timestamp_t> {
            if (tid == txn::id_t{1}) { return txn::timestamp_t{5}; }
            return stdx::none;
        })));
}

TEST_CASE("exec::index_scan options: early abort, non-inclusive, isolation levels") {
    helpers::tempfile file{"exec_scan_index_opts_test"};
    using txn_tree_t   = txn::iot_tree<i64, 128, 64>;
    using index_tree_t = storage::bplus_tree<i64, i64, 64>;

    auto                        pool{UNWRAP(txn_tree_t::tree_t::pool_t::open(file.path))};
    auto                        primary_base{UNWRAP(txn_tree_t::tree_t::create(*pool))};
    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn_tree_t                  primary_tree{primary_base, undo_mgr};
    index_tree_t                secondary_index{UNWRAP(index_tree_t::create(*pool))};
    txn::manager                tm;
    helpers::tempfile           wal_file{"exec_scan_opts_wal"};
    wal::log::manager           lm{wal_file.path, 1_MiB};

    const auto t1{tm.begin_txn()};
    REQUIRE(primary_tree.insert_txn(t1, 101, helpers::span_from_string("rec101")));
    REQUIRE(primary_tree.insert_txn(t1, 102, helpers::span_from_string("rec102")));
    REQUIRE(primary_tree.insert_txn(t1, 103, helpers::span_from_string("rec103")));

    REQUIRE(secondary_index.emplace(10, 101));
    REQUIRE(secondary_index.emplace(20, 102));
    REQUIRE(secondary_index.emplace(30, 103));

    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{1}));
    REQUIRE(tm.commit_txn(t1, lm));

    // READ_COMMITTED scanner
    const auto                    t2{tm.begin_txn(txn::isolation_level_t::READ_COMMITTED)};
    const auto                    snap2{UNWRAP(tm.acquire_snapshot(t2))};
    index_scan<i64, i64, 128, 64> scanner_rc{secondary_index, primary_tree, t2, snap2, tm};

    // Non-inclusive scan: 10 to 30 with inclusive = false (upper bound non-inclusive: 10, 20)
    CHECK(UNWRAP(scanner_rc(10, 30, [&](const i64&, gsl::span<const std::byte>) {}, false)) == 2);

    // Early abort visitor
    const auto                    t3{tm.begin_txn(txn::isolation_level_t::REPEATABLE_READ)};
    const auto                    snap3{UNWRAP(tm.acquire_snapshot(t3))};
    index_scan<i64, i64, 128, 64> scanner_rr{secondary_index, primary_tree, t3, snap3, tm};

    CHECK(UNWRAP(scanner_rr(
              10,
              30,
              [&](const i64&, gsl::span<const std::byte>) -> bool {
                  return false; // Stop after first item
              },
              true)) == 1);
}

} // namespace cairn::tests
