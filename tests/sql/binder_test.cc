#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "sql/binder/binder.hh"
#include "sql/binder/nodes.hh"
#include "sql/catalog.hh"
#include "sql/file.hh"
#include "sql/parser/parser.hh"
#include "sql/schema.hh"
#include "sql/type.hh"
#include "storage/buffer_pool.hh"
#include "support/diagnostic/error.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::sql;
using namespace stdx::size_literals;

namespace {

auto parse_sql(std::string_view query) {
    const file f{query};
    return parser::parse(f);
}

} // namespace

TEST_CASE("sql::binder basic resolution and errors") {
    helpers::tempfile db_file{"binder_test_db"};
    helpers::tempfile log_file{"binder_test_log"};

    using pool_t = storage::buffer_pool<64>;
    wal::log::manager lm{log_file.path, 1_MiB};
    auto              pool{UNWRAP(pool_t::open(db_file.path))};
    pool->set_log_manager(lm);

    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn::manager                tm;
    catalog<64>                 cat{*pool, tm, undo_mgr, lm};
    REQUIRE(cat.bootstrap());

    // Create table "users"
    {
        const auto transaction{tm.begin_txn()};
        schema     user_schema{{column{"id", type::id_t::INTEGER, false},
                                column{"name", type::id_t::VARCHAR, true},
                                column{"active", type::id_t::BOOLEAN, true}}};

        auto meta{UNWRAP(cat.create_table(transaction, table_id_t{10}, "users", user_schema))};
        REQUIRE(tm.update_txn_lsn(transaction, wal::log::seq_num{2}));
        REQUIRE(tm.commit_txn(transaction, lm));
    }

    binder::binder_t<64> b{cat};

    SECTION("Bind SELECT *") {
        auto tree{UNWRAP(parse_sql("SELECT * FROM users;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);

        auto bound_ast{UNWRAP(b.bind(tree, roots[0]))};
        auto bound_roots{bound_ast.roots()};
        REQUIRE(bound_roots.size() == 1);

        auto root_id{bound_roots[0]};
        REQUIRE(root_id.kind() == binder::node_kind_t::SELECT_STMT);

        const auto& select{UNWRAP(bound_ast.get_as_opt<binder::select_stmt_t>(root_id))};
        CHECK(select.table_id == table_id_t{10});
        CHECK(select.table_name == "users");
        CHECK_FALSE(select.table_alias);
        CHECK_FALSE(select.where_clause);

        // Wildcard expanded to 3 columns: id, name, active
        const auto& select_list{select.select_list};
        REQUIRE(select_list.size() == 3);

        {
            CHECK(select_list[0].kind() == binder::node_kind_t::COLUMN_REF_EXPR);
            const auto& col{
                UNWRAP(bound_ast.get_as_opt<binder::column_ref_expr_t>(select_list[0]))};
            CHECK(col.column_name == "id");
            CHECK(col.column_idx == 0);
            CHECK(col.type == type::id_t::INTEGER);
        }

        {
            CHECK(select_list[1].kind() == binder::node_kind_t::COLUMN_REF_EXPR);
            const auto& col{
                UNWRAP(bound_ast.get_as_opt<binder::column_ref_expr_t>(select_list[1]))};
            CHECK(col.column_name == "name");
            CHECK(col.column_idx == 1);
            CHECK(col.type == type::id_t::VARCHAR);
        }

        {
            CHECK(select_list[2].kind() == binder::node_kind_t::COLUMN_REF_EXPR);
            const auto& col{
                UNWRAP(bound_ast.get_as_opt<binder::column_ref_expr_t>(select_list[2]))};
            CHECK(col.column_name == "active");
            CHECK(col.column_idx == 2);
            CHECK(col.type == type::id_t::BOOLEAN);
        }
    }

    SECTION("Bind SELECT with table alias and WHERE clause") {
        auto tree{
            UNWRAP(parse_sql("SELECT u.name, u.id FROM users AS u WHERE u.id = 42 AND u.active;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);

        auto bound_ast{UNWRAP(b.bind(tree, roots[0]))};
        auto bound_roots{bound_ast.roots()};
        REQUIRE(bound_roots.size() == 1);

        auto root_id{bound_roots[0]};
        REQUIRE(root_id.kind() == binder::node_kind_t::SELECT_STMT);

        const auto& select{UNWRAP(bound_ast.get_as_opt<binder::select_stmt_t>(root_id))};
        CHECK(select.table_id == table_id_t{10});
        CHECK(select.table_name == "users");
        CHECK(select.table_alias);
        CHECK(*select.table_alias == "u");

        const auto& select_list{select.select_list};
        REQUIRE(select_list.size() == 2);

        CHECK(select_list[0].kind() == binder::node_kind_t::COLUMN_REF_EXPR);
        const auto& col0{UNWRAP(bound_ast.get_as_opt<binder::column_ref_expr_t>(select_list[0]))};
        CHECK(col0.column_name == "name");
        CHECK(col0.column_idx == 1);

        CHECK(select_list[1].kind() == binder::node_kind_t::COLUMN_REF_EXPR);
        const auto& col1{UNWRAP(bound_ast.get_as_opt<binder::column_ref_expr_t>(select_list[1]))};
        CHECK(col1.column_name == "id");
        CHECK(col1.column_idx == 0);

        auto where_id{UNWRAP(select.where_clause)};
        CHECK(where_id.kind() == binder::node_kind_t::BINARY_EXPR);
        const auto& where{UNWRAP(bound_ast.get_as_opt<binder::binary_expr_t>(where_id))};
        CHECK(where.type == type::id_t::BOOLEAN);
        CHECK(where.op == parser::binary_op_t::AND);

        // LHS of AND (u.id = 42)
        CHECK(where.lhs.kind() == binder::node_kind_t::BINARY_EXPR);
        const auto& eq{UNWRAP(bound_ast.get_as_opt<binder::binary_expr_t>(where.lhs))};
        CHECK(eq.op == parser::binary_op_t::EQUAL);
        CHECK(eq.type == type::id_t::BOOLEAN);

        // RHS of AND (u.active)
        CHECK(where.rhs.kind() == binder::node_kind_t::COLUMN_REF_EXPR);
        const auto& active{UNWRAP(bound_ast.get_as_opt<binder::column_ref_expr_t>(where.rhs))};
        CHECK(active.column_name == "active");
        CHECK(active.type == type::id_t::BOOLEAN);
    }

    SECTION("Bind error: Table not found") {
        auto tree{UNWRAP(parse_sql("SELECT * FROM non_existent;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);
        CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_TABLE_NOT_FOUND);
    }

    SECTION("Bind error: Column not found") {
        auto tree{UNWRAP(parse_sql("SELECT age FROM users;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);
        CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_COLUMN_NOT_FOUND);
    }

    SECTION("Bind error: Table not found when alias masks table name") {
        auto tree{UNWRAP(parse_sql("SELECT users.id FROM users AS u;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);
        CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_TABLE_NOT_FOUND);
    }

    SECTION("Bind error: Type mismatch in WHERE clause") {
        auto tree{UNWRAP(parse_sql("SELECT * FROM users WHERE id;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);
        CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_TYPE_MISMATCH);
    }

    SECTION("Bind error: Type mismatch in binary operations") {
        auto tree{UNWRAP(parse_sql("SELECT * FROM users WHERE name = 5;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);
        CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_TYPE_MISMATCH);
    }
}

} // namespace cairn::tests
