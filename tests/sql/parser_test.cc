#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "sql/ast.hh"
#include "sql/file.hh"
#include "sql/parser.hh"
#include "sql/type.hh"
#include "support/diagnostic/error.hh"
#include "support/diagnostic/list.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::sql;

namespace {

auto parse_sql(std::string_view query, diagnostic_list& diags) {
    const file f{query};
    return parse(f, diags);
}

} // namespace

TEST_CASE("Parse SELECT statement") {
    diagnostic_list diags;
    auto            tree  = UNWRAP(parse_sql("SELECT * FROM users;", diags));
    auto            roots = tree.roots();
    REQUIRE(roots.size() == 1);

    auto root_id = roots[0];
    REQUIRE(root_id.kind == ast::node_kind_t::SELECT_STMT);
    const auto& select{UNWRAP(tree.get_as_opt<ast::select_stmt_t>(root_id))};
    CHECK(select.table_name == "users");

    REQUIRE(select.select_list.size() == 1);
    CHECK(select.select_list[0].is_star);
    CHECK_FALSE(select.where_clause.is_valid());
}

TEST_CASE("Parse SELECT statement with WHERE clause and operators") {
    diagnostic_list diags;
    auto tree  = UNWRAP(parse_sql("SELECT id, name FROM users WHERE id = 5 AND age > 21;", diags));
    auto roots = tree.roots();
    REQUIRE(roots.size() == 1);

    auto root_id = roots[0];
    REQUIRE(root_id.kind == ast::node_kind_t::SELECT_STMT);
    const auto& select{UNWRAP(tree.get_as_opt<ast::select_stmt_t>(root_id))};
    CHECK(select.table_name == "users");

    REQUIRE(select.select_list.size() == 2);
    CHECK_FALSE(select.select_list[0].is_star);
    CHECK_FALSE(select.select_list[1].is_star);

    REQUIRE(select.where_clause.is_valid());
    auto where_id = select.where_clause;
    REQUIRE(where_id.kind == ast::node_kind_t::BINARY_EXPR);
    const auto& binary_and{UNWRAP(tree.get_as_opt<ast::binary_expr_t>(where_id))};
    CHECK(binary_and.op == ast::binary_op_t::AND);

    // LHS of AND (id = 5)
    REQUIRE(binary_and.lhs.kind == ast::node_kind_t::BINARY_EXPR);
    const auto& eq{UNWRAP(tree.get_as_opt<ast::binary_expr_t>(binary_and.lhs))};
    CHECK(eq.op == ast::binary_op_t::EQUAL);

    // RHS of AND (age > 21)
    REQUIRE(binary_and.rhs.kind == ast::node_kind_t::BINARY_EXPR);
    const auto& gt{UNWRAP(tree.get_as_opt<ast::binary_expr_t>(binary_and.rhs))};
    CHECK(gt.op == ast::binary_op_t::GREATER_THAN);
}

TEST_CASE("Parse CREATE TABLE statement") {
    diagnostic_list diags;
    auto            tree  = UNWRAP(parse_sql(
        "CREATE TABLE customers (id INT NOT NULL, name VARCHAR NULL, is_active BOOLEAN);", diags));
    auto            roots = tree.roots();
    REQUIRE(roots.size() == 1);

    auto root_id = roots[0];
    REQUIRE(root_id.kind == ast::node_kind_t::CREATE_TABLE_STMT);
    const auto& create{UNWRAP(tree.get_as_opt<ast::create_table_stmt_t>(root_id))};
    CHECK(create.table_name == "customers");

    auto column_defs = create.column_defs;
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
    diagnostic_list diags;
    auto            tree  = UNWRAP(parse_sql("DROP TABLE customers;", diags));
    auto            roots = tree.roots();
    REQUIRE(roots.size() == 1);

    auto root_id = roots[0];
    REQUIRE(root_id.kind == ast::node_kind_t::DROP_TABLE_STMT);
    const auto& drop{UNWRAP(tree.get_as_opt<ast::drop_table_stmt_t>(root_id))};
    CHECK(drop.table_name == "customers");
}

TEST_CASE("Parse ALTER TABLE statement") {
    diagnostic_list diags;
    auto tree  = UNWRAP(parse_sql("ALTER TABLE employees ADD email VARCHAR NOT NULL;", diags));
    auto roots = tree.roots();
    REQUIRE(roots.size() == 1);

    auto root_id = roots[0];
    REQUIRE(root_id.kind == ast::node_kind_t::ALTER_TABLE_STMT);
    const auto& alter{UNWRAP(tree.get_as_opt<ast::alter_table_stmt_t>(root_id))};
    CHECK(alter.table_name == "employees");
    CHECK(alter.column_def.name == "email");
    CHECK(alter.column_def.type == type::id_t::VARCHAR);
    CHECK_FALSE(alter.column_def.nullable);
}

TEST_CASE("Parse CREATE INDEX statement") {
    diagnostic_list diags;
    auto            tree =
        UNWRAP(parse_sql("CREATE INDEX idx_emp_name ON employees (last_name, first_name);", diags));
    auto roots = tree.roots();
    REQUIRE(roots.size() == 1);

    auto root_id = roots[0];
    REQUIRE(root_id.kind == ast::node_kind_t::CREATE_INDEX_STMT);
    const auto& idx{UNWRAP(tree.get_as_opt<ast::create_index_stmt_t>(root_id))};
    CHECK(idx.index_name == "idx_emp_name");
    CHECK(idx.table_name == "employees");

    auto cols = idx.columns;
    REQUIRE(cols.size() == 2);
    CHECK(cols[0] == "last_name");
    CHECK(cols[1] == "first_name");
}

TEST_CASE("Parse DROP INDEX statement") {
    diagnostic_list diags;
    auto            tree  = UNWRAP(parse_sql("DROP INDEX idx_emp_name ON employees;", diags));
    auto            roots = tree.roots();
    REQUIRE(roots.size() == 1);

    auto root_id = roots[0];
    REQUIRE(root_id.kind == ast::node_kind_t::DROP_INDEX_STMT);
    const auto& drop_idx{UNWRAP(tree.get_as_opt<ast::drop_index_stmt_t>(root_id))};
    CHECK(drop_idx.index_name == "idx_emp_name");
    CHECK(drop_idx.table_name == "employees");
}

TEST_CASE("Parse error diagnostics") {
    diagnostic_list diags;
    auto            err = UNWRAP_ERR(parse_sql("SELECT FROM users;", diags));
    CHECK(err == error::SQL_EOF);

    auto span = gsl::span<const diagnostic>(diags);
    REQUIRE(span.size() == 1);
    CHECK(span[0].get_err() == error::SQL_EOF);

    const auto& loc = UNWRAP(span[0].get_loc());
    CHECK(loc.line == 0);
    CHECK(loc.column == 7);
}

} // namespace cairn::tests
