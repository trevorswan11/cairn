#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "exec/ddl_executor.hh"
#include "exec/table_scan.hh"
#include "sql/binder/binder.hh"
#include "sql/binder/nodes.hh"
#include "sql/catalog.hh"
#include "sql/file.hh"
#include "sql/parser/parser.hh"
#include "sql/schema.hh"
#include "sql/tuple.hh"
#include "sql/type.hh"
#include "sql/value.hh"
#include "storage/bplus.hh"
#include "storage/buffer_pool.hh"
#include "support/diagnostic/error.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::exec;
using namespace stdx::size_literals;

namespace {

auto parse_and_bind(sql::binder::binder_t<64>& b, std::string_view query) {
    const sql::file f{query};
    auto            tree{UNWRAP(sql::parser::parse(f))};
    auto            roots{tree.roots()};
    return b.bind(tree, roots[0]);
}

} // namespace

TEST_CASE("ddl_executor CREATE TABLE, DROP TABLE, ALTER TABLE, and CREATE INDEX") {
    helpers::tempfile db_file{"ddl_executor_test_db"};
    helpers::tempfile log_file{"ddl_executor_test_log"};

    using pool_t = storage::buffer_pool<64>;
    wal::log::manager lm{log_file.path, 1_MiB};
    auto              pool{UNWRAP(pool_t::open(db_file.path))};
    pool->set_log_manager(lm);

    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn::manager                tm;
    sql::catalog<64>            cat{*pool, tm, undo_mgr, lm};
    REQUIRE(cat.bootstrap());

    sql::binder::binder_t<64> b{cat};
    ddl_executor<64>          executor{cat, *pool, tm};

    // CREATE TABLE
    {
        const auto  txn_id{tm.begin_txn()};
        auto        bound_ast{UNWRAP(parse_and_bind(b, "CREATE TABLE users (id INT, age INT);"))};
        const auto& stmt{
            UNWRAP(bound_ast.get_as_opt<sql::binder::create_table_stmt_t>(bound_ast.roots()[0]))};

        const auto table_id{cat.get_next_table_id()};
        REQUIRE(executor.execute_create_table(txn_id, table_id, stmt));

        REQUIRE(tm.update_txn_lsn(txn_id, wal::log::seq_num{2}));
        REQUIRE(tm.commit_txn(txn_id, lm));
    }

    // Verify metadata and insert data
    auto tbl{UNWRAP(cat.get_table("users"))};
    CHECK(tbl.table_schema.column_count() == 2);
    CHECK(tbl.table_schema[0].name() == "id");
    CHECK(tbl.table_schema[1].name() == "age");

    {
        const auto txn_id{tm.begin_txn()};
        using txn_tree_t = txn::iot_tree<i64, 128, 64>;
        typename txn_tree_t::tree_t tree_impl{*pool, tbl.root_page_id};
        txn_tree_t                  primary_tree{tree_impl, undo_mgr};

        std::vector<sql::value_t> vals{sql::value_t{static_cast<i32>(1)},
                                       sql::value_t{static_cast<i32>(25)}};
        auto                      t{UNWRAP(sql::tuple::serialize(tbl.table_schema, vals))};
        REQUIRE(primary_tree.insert_txn(txn_id, 1, t.data()));

        REQUIRE(tm.update_txn_lsn(txn_id, wal::log::seq_num{3}));
        REQUIRE(tm.commit_txn(txn_id, lm));
    }

    // ALTER TABLE ADD COLUMN
    {
        const auto txn_id{tm.begin_txn()};
        auto bound_ast{UNWRAP(parse_and_bind(b, "ALTER TABLE users ADD COLUMN active BOOLEAN;"))};
        const auto& stmt{
            UNWRAP(bound_ast.get_as_opt<sql::binder::alter_table_stmt_t>(bound_ast.roots()[0]))};

        REQUIRE(executor.execute_alter_table(txn_id, stmt));
        REQUIRE(tm.update_txn_lsn(txn_id, wal::log::seq_num{4}));
        REQUIRE(tm.commit_txn(txn_id, lm));
    }

    // Verify schema upgrade and default null active column
    tbl = UNWRAP(cat.get_table("users"));
    CHECK(tbl.table_schema.column_count() == 3);
    CHECK(tbl.table_schema[2].name() == "active");
    CHECK(tbl.table_schema[2].type() == sql::type::id_t::BOOLEAN);

    {
        const auto txn_id{tm.begin_txn()};
        const auto snap{UNWRAP(tm.acquire_snapshot(txn_id))};
        using txn_tree_t = txn::iot_tree<i64, 128, 64>;
        typename txn_tree_t::tree_t tree_impl{*pool, tbl.root_page_id};
        txn_tree_t                  primary_tree{tree_impl, undo_mgr};

        table_scan<i64, 128, 64>               scanner{primary_tree, txn_id, snap, tm};
        std::vector<std::vector<sql::value_t>> rows;

        REQUIRE(scanner(0, 100, [&](const i64&, gsl::span<const std::byte> val) {
                    sql::tuple::byte_buffer buf;
                    buf.resize(val.size());
                    std::memcpy(buf.data(), val.data(), val.size());
                    sql::tuple t{std::move(buf)};

                    std::vector<sql::value_t> vals(tbl.table_schema.column_count());
                    if (t.deserialize(tbl.table_schema, vals)) {
                        rows.emplace_back(std::move(vals));
                    }
                }) == 1);

        REQUIRE(rows.size() == 1);
        CHECK(rows[0][0].get_value().as<i32>() == 1);
        CHECK(rows[0][1].get_value().as<i32>() == 25);
        CHECK(rows[0][2].is_null()); // default NULL
        REQUIRE(tm.commit_txn(txn_id, lm));
    }

    // CREATE INDEX
    {
        const auto txn_id{tm.begin_txn()};
        auto bound_ast{UNWRAP(parse_and_bind(b, "CREATE INDEX idx_users_age ON users (age);"))};
        const auto& stmt{
            UNWRAP(bound_ast.get_as_opt<sql::binder::create_index_stmt_t>(bound_ast.roots()[0]))};

        const auto index_id{cat.get_next_index_id()};
        REQUIRE(executor.execute_create_index(txn_id, index_id, stmt));
        REQUIRE(tm.update_txn_lsn(txn_id, wal::log::seq_num{5}));
        REQUIRE(tm.commit_txn(txn_id, lm));
    }

    // Verify index population and lookup
    {
        auto idx{UNWRAP(cat.get_index("idx_users_age"))};
        CHECK(idx.column_id == 1);

        using index_tree_t = storage::bplus_tree<i64, i64, 64>;
        index_tree_t secondary_index{*pool, idx.root_page_id};
        CHECK(UNWRAP(secondary_index.get(25)) == 1);
    }

    // ALTER TABLE DROP COLUMN
    {
        const auto  txn_id{tm.begin_txn()};
        auto        bound_ast{UNWRAP(parse_and_bind(b, "ALTER TABLE users DROP COLUMN age;"))};
        const auto& stmt{
            UNWRAP(bound_ast.get_as_opt<sql::binder::alter_table_stmt_t>(bound_ast.roots()[0]))};

        REQUIRE(executor.execute_alter_table(txn_id, stmt));
        REQUIRE(tm.update_txn_lsn(txn_id, wal::log::seq_num{6}));
        REQUIRE(tm.commit_txn(txn_id, lm));
    }

    // Verify column drop, catalog updates, and index drop cascade
    tbl = UNWRAP(cat.get_table("users"));
    CHECK(tbl.table_schema.column_count() == 2);
    CHECK(tbl.table_schema[0].name() == "id");
    CHECK(tbl.table_schema[1].name() == "active");
    CHECK_FALSE(cat.get_index("idx_users_age").has_value()); // because "age" column was dropped

    // DROP TABLE
    {
        const auto  txn_id{tm.begin_txn()};
        auto        bound_ast{UNWRAP(parse_and_bind(b, "DROP TABLE users;"))};
        const auto& stmt{
            UNWRAP(bound_ast.get_as_opt<sql::binder::drop_table_stmt_t>(bound_ast.roots()[0]))};

        REQUIRE(executor.execute_drop_table(txn_id, stmt));
        REQUIRE(tm.update_txn_lsn(txn_id, wal::log::seq_num{7}));
        REQUIRE(tm.commit_txn(txn_id, lm));
    }

    CHECK_FALSE(cat.get_table("users").has_value());
}

TEST_CASE("ddl_executor error cases and index column shifting") {
    helpers::tempfile db_file{"ddl_executor_err_db"};
    helpers::tempfile log_file{"ddl_executor_err_log"};

    using pool_t = storage::buffer_pool<64>;
    wal::log::manager lm{log_file.path, 1_MiB};
    auto              pool{UNWRAP(pool_t::open(db_file.path))};
    pool->set_log_manager(lm);

    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn::manager                tm;
    sql::catalog<64>            cat{*pool, tm, undo_mgr, lm};
    REQUIRE(cat.bootstrap());

    ddl_executor<64> executor{cat, *pool, tm};
    const auto       t1{tm.begin_txn()};

    // Non-existent table alter
    sql::binder::alter_table_stmt_t alter_stmt{
        .table_id   = sql::table_id_t{999},
        .table_name = "nonexistent",
        .column_def = {"c1", sql::type::id_t::INTEGER},
    };
    CHECK(UNWRAP_ERR(executor.execute_alter_table(t1, alter_stmt)) == error::SQL_TABLE_NOT_FOUND);

    // Non-existent table create index
    sql::binder::create_index_stmt_t idx_stmt{
        .index_name     = "idx1",
        .table_id       = sql::table_id_t{999},
        .table_name     = "nonexistent",
        .column_names   = {"c1"},
        .column_indices = {0},
    };
    CHECK(UNWRAP_ERR(executor.execute_create_index(t1, sql::index_id_t{10}, idx_stmt)) ==
          error::SQL_TABLE_NOT_FOUND);

    // Empty column indices create index
    sql::binder::create_table_stmt_t create_stmt{
        .table_name  = "t1",
        .column_defs = {{"c1", sql::type::id_t::INTEGER}},
    };
    REQUIRE(executor.execute_create_table(t1, sql::table_id_t{100}, create_stmt));
    idx_stmt.table_id       = sql::table_id_t{100};
    idx_stmt.table_name     = "t1";
    idx_stmt.column_indices = {};
    CHECK(UNWRAP_ERR(executor.execute_create_index(t1, sql::index_id_t{11}, idx_stmt)) ==
          error::SQL_COLUMN_NOT_FOUND);

    // Drop non-existent column
    alter_stmt.table_id   = sql::table_id_t{100};
    alter_stmt.table_name = "t1";
    alter_stmt.column_def = {"nonexistent_col", stdx::none};
    CHECK(UNWRAP_ERR(executor.execute_alter_table(t1, alter_stmt)) == error::SQL_COLUMN_NOT_FOUND);
}

} // namespace cairn::tests
