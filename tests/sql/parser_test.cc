#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "sql/parser/parser.hh"
#include "sql/type.hh"
#include "support/diagnostic/location.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::sql;

TEST_CASE("Parse SELECT statement") {
    auto tree{UNWRAP(parser::parse("SELECT * FROM users;"))};
    auto roots{tree.roots()};
    REQUIRE(roots.size() == 1);

    auto root_id{roots[0]};
    REQUIRE(root_id.kind() == parser::node_kind_t::SELECT_STMT);
    const auto& select{UNWRAP(tree[root_id].as_opt<parser::select_stmt_t>())};
    CHECK(select.table_name == "users");

    REQUIRE(select.select_list.size() == 1);
    CHECK_FALSE(select.select_list[0].expr);
    CHECK_FALSE(select.where_clause);
}

TEST_CASE("Parse SELECT statement with WHERE clause and operators") {
    auto tree{UNWRAP(parser::parse("SELECT id, name FROM users WHERE id = 5 AND age > 21;"))};
    auto roots{tree.roots()};
    REQUIRE(roots.size() == 1);

    auto root_id{roots[0]};
    REQUIRE(root_id.kind() == parser::node_kind_t::SELECT_STMT);
    const auto& select{UNWRAP(tree[root_id].as_opt<parser::select_stmt_t>())};

    CHECK(select.table_name == "users");
    REQUIRE(select.select_list.size() == 2);
    CHECK(select.select_list[0].expr);
    CHECK(select.select_list[1].expr);

    auto where_id{UNWRAP(select.where_clause)};
    REQUIRE(where_id.kind() == parser::node_kind_t::BINARY_EXPR);
    const auto& binary_and{UNWRAP(tree[where_id].as_opt<parser::binary_expr_t>())};
    CHECK(binary_and.op == parser::binary_op_t::AND);

    // LHS of AND (id = 5)
    REQUIRE(binary_and.lhs.kind() == parser::node_kind_t::BINARY_EXPR);
    const auto& eq{UNWRAP(tree[binary_and.lhs].as_opt<parser::binary_expr_t>())};
    CHECK(eq.op == parser::binary_op_t::EQ);

    // RHS of AND (age > 21)
    REQUIRE(binary_and.rhs.kind() == parser::node_kind_t::BINARY_EXPR);
    const auto& gt{UNWRAP(tree[binary_and.rhs].as_opt<parser::binary_expr_t>())};
    CHECK(gt.op == parser::binary_op_t::GT);
}

TEST_CASE("Parse CREATE TABLE statement") {
    auto tree{UNWRAP(parser::parse(
        "CREATE TABLE customers (id INT NOT NULL, name VARCHAR NULL, is_active BOOLEAN);"))};
    auto roots{tree.roots()};
    REQUIRE(roots.size() == 1);

    auto root_id{roots[0]};
    REQUIRE(root_id.kind() == parser::node_kind_t::CREATE_TABLE_STMT);
    const auto& create{UNWRAP(tree[root_id].as_opt<parser::create_table_stmt_t>())};
    CHECK(create.table_name == "customers");

    auto column_defs{create.column_defs};
    REQUIRE(column_defs.size() == 3);

    CHECK(column_defs[0].name == "id");
    CHECK(column_defs[0].type == type::id_t::INTEGER);
    CHECK_FALSE(column_defs[0].nullable);

    CHECK(column_defs[1].name == "name");
    CHECK(column_defs[1].type == type::id_t::VARCHAR);
    CHECK(column_defs[1].nullable);

    CHECK(column_defs[2].name == "is_active");
    CHECK(column_defs[2].type == type::id_t::BOOLEAN);
    CHECK(column_defs[2].nullable);
}

TEST_CASE("Parse DROP TABLE statement") {
    auto tree{UNWRAP(parser::parse("DROP TABLE customers;"))};
    auto roots{tree.roots()};
    REQUIRE(roots.size() == 1);

    auto root_id{roots[0]};
    REQUIRE(root_id.kind() == parser::node_kind_t::DROP_TABLE_STMT);
    const auto& drop{UNWRAP(tree[root_id].as_opt<parser::drop_table_stmt_t>())};
    CHECK(drop.table_name == "customers");
}

TEST_CASE("Parse ALTER TABLE statement") {
    auto tree{UNWRAP(parser::parse("ALTER TABLE employees ADD email VARCHAR NOT NULL;"))};
    auto roots{tree.roots()};
    REQUIRE(roots.size() == 1);

    auto root_id{roots[0]};
    REQUIRE(root_id.kind() == parser::node_kind_t::ALTER_TABLE_STMT);
    const auto& alter{UNWRAP(tree[root_id].as_opt<parser::alter_table_stmt_t>())};
    CHECK(alter.table_name == "employees");
    CHECK(alter.column_def.name == "email");
    CHECK(alter.column_def.type == type::id_t::VARCHAR);
    CHECK_FALSE(alter.column_def.nullable);
}

TEST_CASE("Parse CREATE INDEX statement") {
    auto tree{
        UNWRAP(parser::parse("CREATE INDEX idx_emp_name ON employees (last_name, first_name);"))};
    auto roots{tree.roots()};
    REQUIRE(roots.size() == 1);

    auto root_id{roots[0]};
    REQUIRE(root_id.kind() == parser::node_kind_t::CREATE_INDEX_STMT);
    const auto& idx{UNWRAP(tree[root_id].as_opt<parser::create_index_stmt_t>())};
    CHECK(idx.index_name == "idx_emp_name");
    CHECK(idx.table_name == "employees");

    auto cols{idx.columns};
    REQUIRE(cols.size() == 2);
    CHECK(cols[0] == "last_name");
    CHECK(cols[1] == "first_name");
}

TEST_CASE("Parse DROP INDEX statement") {
    auto tree{UNWRAP(parser::parse("DROP INDEX idx_emp_name ON employees;"))};
    auto roots{tree.roots()};
    REQUIRE(roots.size() == 1);

    auto root_id{roots[0]};
    REQUIRE(root_id.kind() == parser::node_kind_t::DROP_INDEX_STMT);
    const auto& drop_idx{UNWRAP(tree[root_id].as_opt<parser::drop_index_stmt_t>())};
    CHECK(drop_idx.index_name == "idx_emp_name");
    CHECK(drop_idx.table_name == "employees");
}

TEST_CASE("Parse error diagnostics") {
    auto loc{UNWRAP_ERR(parser::parse("SELECT FROM users;"))};
    CHECK(loc.line == 0);
    CHECK(loc.column == 7);
}

TEST_CASE("Parse aggregates, GROUP BY, and HAVING clauses") {
    SECTION("COUNT(*) aggregate") {
        auto tree{UNWRAP(parser::parse("SELECT COUNT(*) FROM users;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);

        const auto& select{UNWRAP(tree[roots[0]].as_opt<parser::select_stmt_t>())};
        CHECK(select.table_name == "users");
        REQUIRE(select.select_list.size() == 1);

        auto        expr_id{UNWRAP(select.select_list[0].expr)};
        const auto& agg{UNWRAP(tree[expr_id].as_opt<parser::aggregate_expr_t>())};
        CHECK(agg.func == parser::agg_func_t::COUNT);
        CHECK_FALSE(agg.arg);
        CHECK_FALSE(agg.is_distinct);
    }

    SECTION("Aggregates with GROUP BY and HAVING") {
        auto tree{UNWRAP(parser::parse(
            "SELECT department_id, SUM(salary), AVG(DISTINCT bonus) FROM employees "
            "WHERE active = true GROUP BY department_id, location_id HAVING SUM(salary) "
            "> 50000;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);

        const auto& select{UNWRAP(tree[roots[0]].as_opt<parser::select_stmt_t>())};
        CHECK(select.table_name == "employees");
        REQUIRE(select.select_list.size() == 3);

        CHECK(select.where_clause);
        REQUIRE(select.group_by.size() == 2);

        auto having_id{UNWRAP(select.having_clause)};
        REQUIRE(having_id.kind() == parser::node_kind_t::BINARY_EXPR);
        const auto& having_bin{UNWRAP(tree[having_id].as_opt<parser::binary_expr_t>())};
        CHECK(having_bin.op == parser::binary_op_t::GT);

        // Check AVG(DISTINCT bonus)
        auto        avg_expr_id{UNWRAP(select.select_list[2].expr)};
        const auto& avg_agg{UNWRAP(tree[avg_expr_id].as_opt<parser::aggregate_expr_t>())};
        CHECK(avg_agg.func == parser::agg_func_t::AVG);
        CHECK(avg_agg.is_distinct);
        CHECK(avg_agg.arg);
    }
}

TEST_CASE("Parse modulo, unary operators, and built-in functions") {
    SECTION("Parse modulo binary operator") {
        auto tree{UNWRAP(parser::parse("SELECT id % 10 FROM users;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);
        const auto& select{UNWRAP(tree[roots[0]].as_opt<parser::select_stmt_t>())};
        auto        expr_id{UNWRAP(select.select_list[0].expr)};
        REQUIRE(expr_id.kind() == parser::node_kind_t::BINARY_EXPR);
        const auto& binary{UNWRAP(tree[expr_id].as_opt<parser::binary_expr_t>())};
        CHECK(binary.op == parser::binary_op_t::MOD);
    }

    SECTION("Parse negation, NOT, IS NULL unary operators") {
        auto tree{UNWRAP(parser::parse(
            "SELECT -id, NOT active FROM users WHERE name IS NULL AND id IS NOT NULL;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);
        const auto& select{UNWRAP(tree[roots[0]].as_opt<parser::select_stmt_t>())};

        // -id
        auto minus_id{UNWRAP(select.select_list[0].expr)};
        REQUIRE(minus_id.kind() == parser::node_kind_t::UNARY_EXPR);
        const auto& unary_minus{UNWRAP(tree[minus_id].as_opt<parser::unary_expr_t>())};
        CHECK(unary_minus.op == parser::unary_op_t::MINUS);

        // NOT active
        auto not_active{UNWRAP(select.select_list[1].expr)};
        REQUIRE(not_active.kind() == parser::node_kind_t::UNARY_EXPR);
        const auto& unary_not{UNWRAP(tree[not_active].as_opt<parser::unary_expr_t>())};
        CHECK(unary_not.op == parser::unary_op_t::NOT);

        // WHERE clause: name IS NULL AND id IS NOT NULL
        auto where_id{UNWRAP(select.where_clause)};
        REQUIRE(where_id.kind() == parser::node_kind_t::BINARY_EXPR);
        const auto& where_bin{UNWRAP(tree[where_id].as_opt<parser::binary_expr_t>())};
        CHECK(where_bin.op == parser::binary_op_t::AND);

        REQUIRE(where_bin.lhs.kind() == parser::node_kind_t::UNARY_EXPR);
        const auto& is_null{UNWRAP(tree[where_bin.lhs].as_opt<parser::unary_expr_t>())};
        CHECK(is_null.op == parser::unary_op_t::IS_NULL);

        REQUIRE(where_bin.rhs.kind() == parser::node_kind_t::UNARY_EXPR);
        const auto& is_not_null{UNWRAP(tree[where_bin.rhs].as_opt<parser::unary_expr_t>())};
        CHECK(is_not_null.op == parser::unary_op_t::IS_NOT_NULL);
    }

    SECTION("Parse built-in functions") {
        auto tree{
            UNWRAP(parser::parse("SELECT COALESCE(name, 'N/A'), SUBSTR(name, 1, 3) FROM users;"))};
        auto roots{tree.roots()};
        REQUIRE(roots.size() == 1);
        const auto& select{UNWRAP(tree[roots[0]].as_opt<parser::select_stmt_t>())};

        // COALESCE(name, 'N/A')
        auto coalesce_id{UNWRAP(select.select_list[0].expr)};
        REQUIRE(coalesce_id.kind() == parser::node_kind_t::FUNCTION_EXPR);
        const auto& coalesce_fn{UNWRAP(tree[coalesce_id].as_opt<parser::function_expr_t>())};
        CHECK(coalesce_fn.name == "COALESCE");
        REQUIRE(coalesce_fn.args.size() == 2);

        // SUBSTR(name, 1, 3)
        auto substr_id{UNWRAP(select.select_list[1].expr)};
        REQUIRE(substr_id.kind() == parser::node_kind_t::FUNCTION_EXPR);
        const auto& substr_fn{UNWRAP(tree[substr_id].as_opt<parser::function_expr_t>())};
        CHECK(substr_fn.name == "SUBSTR");
        REQUIRE(substr_fn.args.size() == 3);
    }
}

} // namespace cairn::tests
