#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "sql/ast.hh"
#include "sql/file.hh"
#include "sql/parser.hh"
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
    auto            stmt{UNWRAP(parse_sql("SELECT * FROM users;", diags))};

    const auto* select_stmt = dynamic_cast<const ast::select_stmt_t*>(&*stmt);
    REQUIRE(select_stmt != nullptr);
    CHECK(select_stmt->table_name() == "users");

    auto select_list = select_stmt->select_list();
    REQUIRE(select_list.size() == 1);
    CHECK(select_list[0].is_star);
    CHECK_FALSE(select_stmt->where_clause().has_value());
}

TEST_CASE("Parse SELECT statement with WHERE clause and operators") {
    diagnostic_list diags;
    auto stmt{UNWRAP(parse_sql("SELECT id, name FROM users WHERE id = 5 AND age > 21;", diags))};

    const auto* select = dynamic_cast<const ast::select_stmt_t*>(&*stmt);
    REQUIRE(select != nullptr);
    CHECK(select->table_name() == "users");

    auto select_list = select->select_list();
    REQUIRE(select_list.size() == 2);
    CHECK_FALSE(select_list[0].is_star);
    CHECK_FALSE(select_list[1].is_star);

    // Verify WHERE clause structure (id = 5 AND age > 21)
    REQUIRE(select->where_clause().has_value());
    const auto& where      = *select->where_clause();
    const auto* binary_and = dynamic_cast<const ast::binary_expr_t*>(&where);
    REQUIRE(binary_and != nullptr);
    CHECK(binary_and->op() == ast::binary_op_t::AND);

    // LHS of AND (id = 5)
    const auto* eq = dynamic_cast<const ast::binary_expr_t*>(&binary_and->lhs());
    REQUIRE(eq != nullptr);
    CHECK(eq->op() == ast::binary_op_t::EQUAL);

    // RHS of AND (age > 21)
    const auto* gt = dynamic_cast<const ast::binary_expr_t*>(&binary_and->rhs());
    REQUIRE(gt != nullptr);
    CHECK(gt->op() == ast::binary_op_t::GREATER_THAN);
}

TEST_CASE("Parse CREATE TABLE statement") {
    diagnostic_list diags;
    auto            stmt{UNWRAP(parse_sql(
        "CREATE TABLE customers (id INT NOT NULL, name VARCHAR NULL, is_active BOOLEAN);", diags))};

    const auto* create = dynamic_cast<const ast::create_table_stmt_t*>(&*stmt);
    REQUIRE(create != nullptr);
    CHECK(create->table_name() == "customers");

    auto column_defs = create->column_defs();
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
    auto            stmt{UNWRAP(parse_sql("DROP TABLE customers;", diags))};

    const auto* drop = dynamic_cast<const ast::drop_table_stmt_t*>(&*stmt);
    REQUIRE(drop != nullptr);
    CHECK(drop->table_name() == "customers");
}

TEST_CASE("Parse ALTER TABLE statement") {
    diagnostic_list diags;
    auto stmt{UNWRAP(parse_sql("ALTER TABLE employees ADD email VARCHAR NOT NULL;", diags))};

    const auto* alter = dynamic_cast<const ast::alter_table_stmt_t*>(&*stmt);
    REQUIRE(alter != nullptr);
    CHECK(alter->table_name() == "employees");
    CHECK(alter->column_def().name == "email");
    CHECK(alter->column_def().type == type::id_t::VARCHAR);
    CHECK_FALSE(alter->column_def().nullable);
}

TEST_CASE("Parse CREATE INDEX statement") {
    diagnostic_list diags;
    auto            stmt{UNWRAP(
        parse_sql("CREATE INDEX idx_emp_name ON employees (last_name, first_name);", diags))};

    const auto* idx = dynamic_cast<const ast::create_index_stmt_t*>(&*stmt);
    REQUIRE(idx != nullptr);
    CHECK(idx->index_name() == "idx_emp_name");
    CHECK(idx->table_name() == "employees");

    auto cols = idx->columns();
    REQUIRE(cols.size() == 2);
    CHECK(cols[0] == "last_name");
    CHECK(cols[1] == "first_name");
}

TEST_CASE("Parse DROP INDEX statement") {
    diagnostic_list diags;
    auto            stmt{UNWRAP(parse_sql("DROP INDEX idx_emp_name ON employees;", diags))};

    const auto* drop_idx = dynamic_cast<const ast::drop_index_stmt_t*>(&*stmt);
    REQUIRE(drop_idx != nullptr);
    CHECK(drop_idx->index_name() == "idx_emp_name");
    CHECK(drop_idx->table_name() == "employees");
}

TEST_CASE("Parse error diagnostics") {
    diagnostic_list diags;
    CHECK(UNWRAP_ERR(parse_sql("SELECT FROM users;", diags)) == error::SQL_EOF);

    auto span = gsl::span<const diagnostic>(diags);
    REQUIRE(span.size() == 1);
    CHECK(span[0].get_err() == error::SQL_EOF);

    // PEGTL must fail where the "FROM" keyword or list was expected
    REQUIRE(span[0].get_loc().has_value());
    const auto loc = *span[0].get_loc();
    CHECK(loc.line == 0);
    CHECK(loc.column == 7); // SELECT[7] (starts at FROM)
}

} // namespace cairn::tests
