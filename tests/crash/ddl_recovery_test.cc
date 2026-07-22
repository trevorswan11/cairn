#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "exec/ddl_executor.hh"
#include "sql/binder/binder.hh"
#include "sql/binder/nodes.hh"
#include "sql/catalog.hh"
#include "sql/file.hh"
#include "sql/parser/parser.hh"
#include "sql/schema.hh"
#include "sql/tuple.hh"
#include "sql/value.hh"
#include "storage/buffer_pool.hh"
#include "support/crash/injection.hh"
#include "testhelpers/argv.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/subprocess.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/reader.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"
#include "wal/recovery/manager.hh"

namespace cairn::tests {

using namespace stdx::size_literals;

namespace {

auto parse_and_bind(sql::binder::binder_t<64>& b, std::string_view query) {
    const sql::file f{query};
    auto            tree{UNWRAP(sql::parser::parse(f))};
    auto            roots{tree.roots()};
    return b.bind(tree, roots[0]);
}

using pool_t     = storage::buffer_pool<64>;
using txn_tree_t = txn::iot_tree<i64, 128, 64>;

} // namespace

TEST_CASE("ddl crash recovery workload", "[.][ddl_crash]") {
    const auto db_path{helpers::get_env("CAIRN_DB_PATH")};
    const auto wal_path{helpers::get_env("CAIRN_WAL_PATH")};
    const auto limit_str{helpers::get_env("CAIRN_CRASH_LIMIT")};

    const auto limit{UNWRAP(helpers::parse_integral<i32>(limit_str))};
    crash::initialize();
    crash::configure(limit);

    wal::log::manager log{wal_path, 1_MiB};
    auto              bp{UNWRAP(pool_t::open(db_path))};
    bp->set_log_manager(log);

    txn::undo::manager<i64, 64> undo_mgr{*bp};
    txn::manager                tm;
    sql::catalog<64>            cat{*bp, tm, undo_mgr, log};
    REQUIRE(cat.bootstrap());
    REQUIRE(bp->flush());

    sql::binder::binder_t<64> b{cat};
    exec::ddl_executor<64>    executor{cat, *bp, tm};

    // T1: Create Table
    const auto  t1{tm.begin_txn()};
    auto        bound_ast1{UNWRAP(parse_and_bind(b, "CREATE TABLE users (id INT, age INT);"))};
    const auto& stmt1{
        UNWRAP(bound_ast1.get_as_opt<sql::binder::create_table_stmt_t>(bound_ast1.roots()[0]))};

    REQUIRE(executor.execute_create_table(t1, sql::table_id_t{10}, stmt1));
    REQUIRE(bp->flush());
    REQUIRE(tm.update_txn_lsn(t1, wal::log::seq_num{2}));
    REQUIRE(tm.commit_txn(t1, log));

    // T2: Insert Row
    const auto                  t2{tm.begin_txn()};
    auto                        tbl{UNWRAP(cat.get_table("users"))};
    typename txn_tree_t::tree_t tree_impl{*bp, tbl.root_page_id};
    txn_tree_t                  primary_tree{tree_impl, undo_mgr};

    std::vector<sql::value_t> vals{sql::value_t{static_cast<i32>(1)},
                                   sql::value_t{static_cast<i32>(30)}};
    auto                      tup{UNWRAP(sql::tuple::serialize(tbl.table_schema, vals))};
    REQUIRE(primary_tree.insert_txn(t2, 1, tup.data()));
    REQUIRE(bp->flush());
    REQUIRE(tm.update_txn_lsn(t2, wal::log::seq_num{3}));
    REQUIRE(tm.commit_txn(t2, log));

    // T3: Alter Table
    const auto t3{tm.begin_txn()};
    auto bound_ast3{UNWRAP(parse_and_bind(b, "ALTER TABLE users ADD COLUMN active BOOLEAN;"))};
    const auto& stmt3{
        UNWRAP(bound_ast3.get_as_opt<sql::binder::alter_table_stmt_t>(bound_ast3.roots()[0]))};

    REQUIRE(executor.execute_alter_table(t3, stmt3));
    REQUIRE(bp->flush());
    REQUIRE(tm.update_txn_lsn(t3, wal::log::seq_num{4}));
    REQUIRE(tm.commit_txn(t3, log));
}

TEST_CASE("ddl crash recovery entrypoint") {
    const auto self_exe{helpers::self_exe_path()};
    i32        limit{1};
    bool       finished{false};

    while (!finished) {
        helpers::tempfile db_file{"ddl_crash_recovery_db"};
        helpers::set_env("CAIRN_DB_PATH", db_file.path.string());
        helpers::tempfile log_file{"ddl_crash_recovery_log"};
        helpers::set_env("CAIRN_WAL_PATH", log_file.path.string());
        helpers::tempfile control_file{"ddl_crash_recovery_control"};

        helpers::set_env("CAIRN_CRASH_LIMIT", std::to_string(limit));
        const helpers::Argv args{self_exe, "[ddl_crash]"};
        const auto          exit_code{UNWRAP(helpers::spawn_child(args))};

        if (exit_code == 0) {
            finished = true;
        } else {
            // Run recovery and verify consistency on crash
            {
                wal::log::manager log{log_file.path, 1_MiB};
                auto              bp{UNWRAP(pool_t::open(db_file.path))};
                bp->set_log_manager(log);

                txn::manager               tm;
                wal::recovery::manager<64> rm{*bp, tm, log, control_file.path, log_file.path};
                REQUIRE(rm.recover());
            }

            // Verify recovered catalog/table pages match the committed txn state
            {
                wal::log::manager log{log_file.path, 1_MiB};
                auto              bp{UNWRAP(pool_t::open(db_file.path))};
                bp->set_log_manager(log);

                auto reader{UNWRAP(wal::log::reader::open(log_file.path))};
                REQUIRE(reader.seek_to_start());

                bool bootstrap_committed{false};
                bool t1_committed{false};
                bool t3_committed{false};

                while (auto rec{UNWRAP(reader.next_record_lenient())}) {
                    if (rec->type == wal::log::record_type::COMMIT) {
                        if (rec->txn_id == txn::id_t{1}) { bootstrap_committed = true; }
                        if (rec->txn_id == txn::id_t{2}) { t1_committed = true; }
                        if (rec->txn_id == txn::id_t{4}) { t3_committed = true; }
                    }
                }

                if (!bootstrap_committed) {
                    limit++;
                    continue;
                }

                txn::undo::manager<i64, 64> undo_mgr{*bp};
                txn::manager                tm;
                tm.set_next_txn_id(txn::id_t{100});
                sql::catalog<64> cat{*bp, tm, undo_mgr, log};
                REQUIRE(cat.bootstrap());
                REQUIRE(bp->flush());

                if (t1_committed) {
                    const auto& tbl{UNWRAP(cat.get_table("users"))};
                    if (t3_committed) {
                        CHECK(tbl.table_schema.column_count() == 3);
                        CHECK(tbl.table_schema[2].name() == "active");
                    } else {
                        CHECK(tbl.table_schema.column_count() == 2);
                    }
                }
            }

            limit++;
        }
    }
}

} // namespace cairn::tests
