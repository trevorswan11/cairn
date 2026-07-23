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
        CHECK(eq.op == parser::binary_op_t::EQ);
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

    SECTION("Bind aggregates, GROUP BY, HAVING, and coercion") {
        {
            const auto transaction{tm.begin_txn()};
            schema     emp_schema{{column{"id", type::id_t::INTEGER, false},
                                   column{"name", type::id_t::VARCHAR, true},
                                   column{"salary", type::id_t::DOUBLE, false},
                                   column{"dept_id", type::id_t::INTEGER, false}}};

            auto meta{
                UNWRAP(cat.create_table(transaction, table_id_t{20}, "employees", emp_schema))};
            REQUIRE(tm.update_txn_lsn(transaction, wal::log::seq_num{3}));
            REQUIRE(tm.commit_txn(transaction, lm));
        }

        SECTION("Implicit coercion cast injection") {
            auto        tree{UNWRAP(parse_sql("SELECT id FROM employees WHERE salary > 50;"))};
            auto        roots{tree.roots()};
            auto        bound_ast{UNWRAP(b.bind(tree, roots[0]))};
            const auto& select{
                UNWRAP(bound_ast.get_as_opt<binder::select_stmt_t>(bound_ast.roots()[0]))};
            auto        where_id{UNWRAP(select.where_clause)};
            const auto& gt{UNWRAP(bound_ast.get_as_opt<binder::binary_expr_t>(where_id))};
            CHECK(gt.type == type::id_t::BOOLEAN);
            CHECK(gt.rhs.kind() == binder::node_kind_t::CAST_EXPR);
            const auto& cast{UNWRAP(bound_ast.get_as_opt<binder::cast_expr_t>(gt.rhs))};
            CHECK(cast.target_type == type::id_t::DOUBLE);
        }

        SECTION("Aggregate functions and return types") {
            auto tree{
                UNWRAP(parse_sql("SELECT COUNT(*), SUM(salary), AVG(salary) FROM employees;"))};
            auto        roots{tree.roots()};
            auto        bound_ast{UNWRAP(b.bind(tree, roots[0]))};
            const auto& select{
                UNWRAP(bound_ast.get_as_opt<binder::select_stmt_t>(bound_ast.roots()[0]))};
            REQUIRE(select.select_list.size() == 3);

            const auto& count_node{
                UNWRAP(bound_ast.get_as_opt<binder::aggregate_expr_t>(select.select_list[0]))};
            CHECK(count_node.func == parser::agg_func_t::COUNT);
            CHECK(count_node.return_type == type::id_t::BIGINT);
            CHECK_FALSE(count_node.arg);

            const auto& sum_node{
                UNWRAP(bound_ast.get_as_opt<binder::aggregate_expr_t>(select.select_list[1]))};
            CHECK(sum_node.func == parser::agg_func_t::SUM);
            CHECK(sum_node.return_type == type::id_t::DOUBLE);

            const auto& avg_node{
                UNWRAP(bound_ast.get_as_opt<binder::aggregate_expr_t>(select.select_list[2]))};
            CHECK(avg_node.func == parser::agg_func_t::AVG);
            CHECK(avg_node.return_type == type::id_t::DOUBLE);
        }

        SECTION("GROUP BY and HAVING clauses") {
            auto        tree{UNWRAP(parse_sql("SELECT dept_id, SUM(salary) FROM employees GROUP BY "
                                              "dept_id HAVING SUM(salary) > 1000;"))};
            auto        roots{tree.roots()};
            auto        bound_ast{UNWRAP(b.bind(tree, roots[0]))};
            const auto& select{
                UNWRAP(bound_ast.get_as_opt<binder::select_stmt_t>(bound_ast.roots()[0]))};
            REQUIRE(select.group_by.size() == 1);
            CHECK(select.having_clause);
        }

        SECTION("Bind error: Invalid aggregate in WHERE clause") {
            auto tree{UNWRAP(parse_sql("SELECT id FROM employees WHERE SUM(salary) > 1000;"))};
            auto roots{tree.roots()};
            CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_INVALID_AGGREGATE);
        }

        SECTION("Bind error: Duplicate column constraint in CREATE TABLE") {
            auto tree{UNWRAP(parse_sql("CREATE TABLE dup_test (id INT, id INT);"))};
            auto roots{tree.roots()};
            CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_CONSTRAINT_VIOLATION);
        }

        SECTION("Bind error: Invalid aggregate on non-numeric type") {
            auto tree{UNWRAP(parse_sql("SELECT SUM(name) FROM employees;"))};
            auto roots{tree.roots()};
            CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_INVALID_AGGREGATE);
        }

        SECTION("Bind error: Ungrouped column error") {
            auto tree{UNWRAP(parse_sql("SELECT name, SUM(salary) FROM employees;"))};
            auto roots{tree.roots()};
            CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_UNGROUPED_COLUMN);
        }

        SECTION("Bind error: Non-boolean HAVING clause") {
            auto tree{
                UNWRAP(parse_sql("SELECT dept_id FROM employees GROUP BY dept_id HAVING salary;"))};
            auto roots{tree.roots()};
            CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_TYPE_MISMATCH);
        }

        SECTION("Bind DDL error paths: DROP TABLE, ALTER TABLE, INDEX") {
            // DROP TABLE non-existent
            {
                auto tree{UNWRAP(parse_sql("DROP TABLE nonexistent_tbl;"))};
                auto roots{tree.roots()};
                CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_TABLE_NOT_FOUND);
            }

            // ALTER TABLE ADD existing column
            {
                auto tree{UNWRAP(parse_sql("ALTER TABLE employees ADD COLUMN name VARCHAR;"))};
                auto roots{tree.roots()};
                CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_CONSTRAINT_VIOLATION);
            }

            // ALTER TABLE DROP non-existent column
            {
                auto tree{UNWRAP(parse_sql("ALTER TABLE employees DROP COLUMN nonexistent_col;"))};
                auto roots{tree.roots()};
                CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_COLUMN_NOT_FOUND);
            }

            // CREATE INDEX non-existent column
            {
                auto tree{
                    UNWRAP(parse_sql("CREATE INDEX idx_emp_bogus ON employees (bogus_col);"))};
                auto roots{tree.roots()};
                CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_COLUMN_NOT_FOUND);
            }

            // DROP INDEX non-existent table
            {
                auto tree{UNWRAP(parse_sql("DROP INDEX idx_bogus ON nonexistent_tbl;"))};
                auto roots{tree.roots()};
                CHECK(UNWRAP_ERR(b.bind(tree, roots[0])).err() == error::SQL_TABLE_NOT_FOUND);
            }
        }

        SECTION("Bind modulo, unary operators, and built-in functions") {
            // Modulo
            {
                auto        tree{UNWRAP(parse_sql("SELECT id % 10 FROM users;"))};
                auto        roots{tree.roots()};
                auto        bound_ast{UNWRAP(b.bind(tree, roots[0]))};
                auto        bound_roots{bound_ast.roots()};
                const auto& select{
                    UNWRAP(bound_ast.get_as_opt<binder::select_stmt_t>(bound_roots[0]))};
                auto expr_id{select.select_list[0]};
                REQUIRE(expr_id.kind() == binder::node_kind_t::BINARY_EXPR);
                const auto& bin{UNWRAP(bound_ast.get_as_opt<binder::binary_expr_t>(expr_id))};
                CHECK(bin.op == parser::binary_op_t::MOD);
                CHECK(bin.type == type::id_t::INTEGER);
            }

            // Unary MINUS and NOT
            {
                auto        tree{UNWRAP(parse_sql("SELECT -id FROM users WHERE NOT active;"))};
                auto        roots{tree.roots()};
                auto        bound_ast{UNWRAP(b.bind(tree, roots[0]))};
                auto        bound_roots{bound_ast.roots()};
                const auto& select{
                    UNWRAP(bound_ast.get_as_opt<binder::select_stmt_t>(bound_roots[0]))};

                auto select_expr_id{select.select_list[0]};
                REQUIRE(select_expr_id.kind() == binder::node_kind_t::UNARY_EXPR);
                const auto& unary_minus{
                    UNWRAP(bound_ast.get_as_opt<binder::unary_expr_t>(select_expr_id))};
                CHECK(unary_minus.op == binder::unary_op_t::MINUS);
                CHECK(unary_minus.type == type::id_t::INTEGER);

                auto where_id{UNWRAP(select.where_clause)};
                REQUIRE(where_id.kind() == binder::node_kind_t::UNARY_EXPR);
                const auto& unary_not{UNWRAP(bound_ast.get_as_opt<binder::unary_expr_t>(where_id))};
                CHECK(unary_not.op == binder::unary_op_t::NOT);
                CHECK(unary_not.type == type::id_t::BOOLEAN);
            }

            // IS NULL and IS NOT NULL
            {
                auto        tree{UNWRAP(
                    parse_sql("SELECT name FROM users WHERE name IS NULL AND id IS NOT NULL;"))};
                auto        roots{tree.roots()};
                auto        bound_ast{UNWRAP(b.bind(tree, roots[0]))};
                auto        bound_roots{bound_ast.roots()};
                const auto& select{
                    UNWRAP(bound_ast.get_as_opt<binder::select_stmt_t>(bound_roots[0]))};

                auto where_id{UNWRAP(select.where_clause)};
                REQUIRE(where_id.kind() == binder::node_kind_t::BINARY_EXPR);
                const auto& bin_and{UNWRAP(bound_ast.get_as_opt<binder::binary_expr_t>(where_id))};

                REQUIRE(bin_and.lhs.kind() == binder::node_kind_t::UNARY_EXPR);
                const auto& is_null{
                    UNWRAP(bound_ast.get_as_opt<binder::unary_expr_t>(bin_and.lhs))};
                CHECK(is_null.op == binder::unary_op_t::IS_NULL);
                CHECK(is_null.type == type::id_t::BOOLEAN);

                REQUIRE(bin_and.rhs.kind() == binder::node_kind_t::UNARY_EXPR);
                const auto& is_not_null{
                    UNWRAP(bound_ast.get_as_opt<binder::unary_expr_t>(bin_and.rhs))};
                CHECK(is_not_null.op == binder::unary_op_t::IS_NOT_NULL);
                CHECK(is_not_null.type == type::id_t::BOOLEAN);
            }

            // Built-in functions COALESCE, LOWER, SUBSTR
            {
                auto        tree{UNWRAP(parse_sql("SELECT COALESCE(name, 'default'), LOWER(name), "
                                                  "SUBSTR(name, 1, 3) FROM users;"))};
                auto        roots{tree.roots()};
                auto        bound_ast{UNWRAP(b.bind(tree, roots[0]))};
                auto        bound_roots{bound_ast.roots()};
                const auto& select{
                    UNWRAP(bound_ast.get_as_opt<binder::select_stmt_t>(bound_roots[0]))};

                // COALESCE(name, 'default') -> VARCHAR
                auto coalesce_id{select.select_list[0]};
                REQUIRE(coalesce_id.kind() == binder::node_kind_t::FUNCTION_EXPR);
                const auto& coalesce_fn{
                    UNWRAP(bound_ast.get_as_opt<binder::function_expr_t>(coalesce_id))};
                CHECK(coalesce_fn.func == binder::func_type_t::COALESCE);
                CHECK(coalesce_fn.type == type::id_t::VARCHAR);

                // LOWER(name) -> VARCHAR
                auto lower_id{select.select_list[1]};
                REQUIRE(lower_id.kind() == binder::node_kind_t::FUNCTION_EXPR);
                const auto& lower_fn{
                    UNWRAP(bound_ast.get_as_opt<binder::function_expr_t>(lower_id))};
                CHECK(lower_fn.func == binder::func_type_t::LOWER);
                CHECK(lower_fn.type == type::id_t::VARCHAR);

                // SUBSTR(name, 1, 3) -> VARCHAR
                auto substr_id{select.select_list[2]};
                REQUIRE(substr_id.kind() == binder::node_kind_t::FUNCTION_EXPR);
                const auto& substr_fn{
                    UNWRAP(bound_ast.get_as_opt<binder::function_expr_t>(substr_id))};
                CHECK(substr_fn.func == binder::func_type_t::SUBSTR);
                CHECK(substr_fn.type == type::id_t::VARCHAR);
            }
        }
    }
}

} // namespace cairn::tests
