#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "exec/expression_evaluator.hh"
#include "sql/binder/binder.hh"
#include "sql/binder/nodes.hh"
#include "sql/catalog.hh"
#include "sql/file.hh"
#include "sql/parser/parser.hh"
#include "sql/schema.hh"
#include "sql/tuple.hh"
#include "sql/type.hh"
#include "sql/value.hh"
#include "storage/buffer_pool.hh"
#include "support/diagnostic/error.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::exec;
using namespace stdx::size_literals;

namespace {

struct test_context_t {
    helpers::tempfile db_file{"evaluator_test_db"};
    helpers::tempfile log_file{"evaluator_test_log"};
    using pool_t = storage::buffer_pool<64>;
    wal::log::manager           lm;
    stdx::box<pool_t>           pool;
    txn::undo::manager<i64, 64> undo_mgr;
    txn::manager                tm;
    sql::catalog<64>            cat;
    sql::schema                 user_schema;
    sql::binder::binder_t<64>   b;

    test_context_t()
        : lm{log_file.path, 1_MiB}, pool{UNWRAP(pool_t::open(db_file.path))}, undo_mgr{*pool},
          cat{*pool, tm, undo_mgr, lm},
          user_schema{{sql::column{"id", sql::type::id_t::INTEGER, false},
                       sql::column{"name", sql::type::id_t::VARCHAR, true},
                       sql::column{"age", sql::type::id_t::INTEGER, true},
                       sql::column{"salary", sql::type::id_t::DOUBLE, true},
                       sql::column{"active", sql::type::id_t::BOOLEAN, true}}},
          b{cat} {
        pool->set_log_manager(lm);
        REQUIRE(cat.bootstrap());

        const auto transaction{tm.begin_txn()};
        auto meta{UNWRAP(cat.create_table(transaction, sql::table_id_t{10}, "users", user_schema))};
        REQUIRE(tm.update_txn_lsn(transaction, wal::log::seq_num{2}));
        REQUIRE(tm.commit_txn(transaction, lm));
    }

    auto evaluate(std::string_view query, const sql::tuple& t) -> result<sql::value_t> {
        const sql::file f{query};
        auto            tree{UNWRAP(sql::parser::parse(f))};
        auto            bound_ast{UNWRAP(b.bind(tree, tree.roots()[0]))};
        const auto&     select{
            UNWRAP(bound_ast.get_as_opt<sql::binder::select_stmt_t>(bound_ast.roots()[0]))};
        auto bound_expr_id{select.select_list[0]};

        expression_evaluator_t evaluator{bound_ast, user_schema};
        return evaluator.evaluate(bound_expr_id, t);
    }
};

} // namespace

TEST_CASE("exec::expression_evaluator literals, column references, and casts") {
    test_context_t ctx;

    // Create a record: id = 10, name = "Alice", age = NULL, salary = 1250.50, active = TRUE
    std::vector<sql::value_t> values{sql::value_t{i32{10}, false},
                                     sql::value_t{std::string_view{"Alice"}},
                                     sql::value_t::make_null(sql::type::id_t::INTEGER),
                                     sql::value_t{f64{1250.50}},
                                     sql::value_t{true}};
    sql::tuple                t{UNWRAP(sql::tuple::serialize(ctx.user_schema, values))};

    SECTION("Literal expressions") {
        auto val{UNWRAP(ctx.evaluate("SELECT 42 FROM users;", t))};
        CHECK_FALSE(val.is_null());
        CHECK(val.get_value().as<i64>() == 42);

        auto name_lit{UNWRAP(ctx.evaluate("SELECT 'Bob' FROM users;", t))};
        CHECK_FALSE(name_lit.is_null());
        CHECK(name_lit.get_value().as<std::string_view>() == "Bob");

        auto null_lit{UNWRAP(ctx.evaluate("SELECT NULL FROM users;", t))};
        CHECK(null_lit.is_null());
    }

    SECTION("Column references") {
        auto id_val{UNWRAP(ctx.evaluate("SELECT id FROM users;", t))};
        CHECK_FALSE(id_val.is_null());
        CHECK(id_val.get_value().as<i32>() == 10);

        auto name_val{UNWRAP(ctx.evaluate("SELECT name FROM users;", t))};
        CHECK_FALSE(name_val.is_null());
        CHECK(name_val.get_value().as<std::string_view>() == "Alice");

        auto age_val{UNWRAP(ctx.evaluate("SELECT age FROM users;", t))};
        CHECK(age_val.is_null());
    }

    SECTION("Casts") {
        // Integer to Double
        auto to_double{UNWRAP(ctx.evaluate("SELECT CAST(id AS DOUBLE) FROM users;", t))};
        CHECK_FALSE(to_double.is_null());
        CHECK(to_double.get_value().as<f64>() == 10.0);
        CHECK(to_double.type() == sql::type::id_t::DOUBLE);

        // VARCHAR to BOOLEAN
        auto to_bool{UNWRAP(ctx.evaluate("SELECT CAST('true' AS BOOLEAN) FROM users;", t))};
        CHECK_FALSE(to_bool.is_null());
        CHECK(to_bool.get_value().as<bool>() == true);

        // Numeric to VARCHAR
        auto to_varchar{UNWRAP(ctx.evaluate("SELECT CAST(id AS VARCHAR) FROM users;", t))};
        CHECK_FALSE(to_varchar.is_null());
        CHECK(to_varchar.get_value().as<std::string_view>() == "10");
        CHECK(to_varchar.type() == sql::type::id_t::VARCHAR);

        // Null cast propagation
        auto null_cast{UNWRAP(ctx.evaluate("SELECT CAST(age AS DOUBLE) FROM users;", t))};
        CHECK(null_cast.is_null());
        CHECK(null_cast.type() == sql::type::id_t::DOUBLE);
    }
}

TEST_CASE("exec::expression_evaluator unary and binary operators") {
    test_context_t ctx;

    // Create a record: id = 10, name = "Alice", age = NULL, salary = 1250.50, active = TRUE
    std::vector<sql::value_t> values{sql::value_t{i32{10}, false},
                                     sql::value_t{std::string_view{"Alice"}},
                                     sql::value_t::make_null(sql::type::id_t::INTEGER),
                                     sql::value_t{f64{1250.50}},
                                     sql::value_t{true}};
    sql::tuple                t{UNWRAP(sql::tuple::serialize(ctx.user_schema, values))};

    SECTION("Unary operators") {
        // Negation
        auto minus_id{UNWRAP(ctx.evaluate("SELECT -id FROM users;", t))};
        CHECK(minus_id.get_value().as<i32>() == -10);

        // Null negation
        auto minus_age{UNWRAP(ctx.evaluate("SELECT -age FROM users;", t))};
        CHECK(minus_age.is_null());

        // Logical NOT
        auto not_active{UNWRAP(ctx.evaluate("SELECT NOT active FROM users;", t))};
        CHECK(not_active.get_value().as<bool>() == false);

        // Null checks
        auto age_is_null{UNWRAP(ctx.evaluate("SELECT age IS NULL FROM users;", t))};
        CHECK(age_is_null.get_value().as<bool>() == true);

        auto id_is_not_null{UNWRAP(ctx.evaluate("SELECT id IS NOT NULL FROM users;", t))};
        CHECK(id_is_not_null.get_value().as<bool>() == true);
    }

    SECTION("Arithmetic binary operators (including modulo %)") {
        // Basic arithmetic
        auto add{UNWRAP(ctx.evaluate("SELECT id + 5 FROM users;", t))};
        CHECK(add.get_value().as<i32>() == 15);

        auto mul{UNWRAP(ctx.evaluate("SELECT id * 2.5 FROM users;", t))};
        CHECK(mul.get_value().as<f64>() == 25.0);

        // Modulo (%)
        auto mod_int{UNWRAP(ctx.evaluate("SELECT id % 3 FROM users;", t))};
        CHECK(mod_int.get_value().as<i32>() == 1);

        auto mod_double{UNWRAP(ctx.evaluate("SELECT salary % 500.0 FROM users;", t))};
        CHECK(mod_double.get_value().as<f64>() == 250.50);

        // Modulo/Division by zero should evaluate to NULL
        auto div_zero{UNWRAP(ctx.evaluate("SELECT id / 0 FROM users;", t))};
        CHECK(div_zero.is_null());

        auto mod_zero{UNWRAP(ctx.evaluate("SELECT id % 0 FROM users;", t))};
        CHECK(mod_zero.is_null());
    }

    SECTION("Comparison binary operators") {
        auto eq{UNWRAP(ctx.evaluate("SELECT id = 10 FROM users;", t))};
        CHECK(eq.get_value().as<bool>() == true);

        auto ne{UNWRAP(ctx.evaluate("SELECT id <> 5 FROM users;", t))};
        CHECK(ne.get_value().as<bool>() == true);

        auto lt{UNWRAP(ctx.evaluate("SELECT id < 5 FROM users;", t))};
        CHECK(lt.get_value().as<bool>() == false);

        // Comparisons with NULL should propagate to NULL
        auto cmp_null{UNWRAP(ctx.evaluate("SELECT age = 10 FROM users;", t))};
        CHECK(cmp_null.is_null());
    }

    SECTION("Logical binary operators (Three-Valued Logic)") {
        // True AND True -> True
        auto t_and_t{UNWRAP(ctx.evaluate("SELECT active AND active FROM users;", t))};
        CHECK(t_and_t.get_value().as<bool>() == true);

        // True AND False -> False
        auto t_and_f{UNWRAP(ctx.evaluate("SELECT active AND NOT active FROM users;", t))};
        CHECK(t_and_f.get_value().as<bool>() == false);

        // True AND Null -> Null
        auto t_and_n{UNWRAP(
            ctx.evaluate("SELECT active AND (age IS NULL AND active = false) FROM users;", t))};
        auto t_and_null{
            UNWRAP(ctx.evaluate("SELECT active AND (CAST(NULL AS BOOLEAN)) FROM users;", t))};
        CHECK(t_and_null.is_null());

        // False AND Null -> False
        auto f_and_null{
            UNWRAP(ctx.evaluate("SELECT (NOT active) AND (CAST(NULL AS BOOLEAN)) FROM users;", t))};
        CHECK(f_and_null.get_value().as<bool>() == false);

        // True OR Null -> True
        auto t_or_null{
            UNWRAP(ctx.evaluate("SELECT active OR (CAST(NULL AS BOOLEAN)) FROM users;", t))};
        CHECK(t_or_null.get_value().as<bool>() == true);

        // False OR Null -> Null
        auto f_or_null{
            UNWRAP(ctx.evaluate("SELECT (NOT active) OR (CAST(NULL AS BOOLEAN)) FROM users;", t))};
        CHECK(f_or_null.is_null());
    }
}

TEST_CASE("exec::expression_evaluator built-in scalar functions") {
    test_context_t ctx;

    // Create a record: id = 10, name = "Alice", age = NULL, salary = 1250.50, active = TRUE
    std::vector<sql::value_t> values{sql::value_t{i32{10}, false},
                                     sql::value_t{std::string_view{"Alice"}},
                                     sql::value_t::make_null(sql::type::id_t::INTEGER),
                                     sql::value_t{f64{1250.50}},
                                     sql::value_t{true}};
    sql::tuple                t{UNWRAP(sql::tuple::serialize(ctx.user_schema, values))};

    SECTION("COALESCE") {
        auto coal1{UNWRAP(ctx.evaluate("SELECT COALESCE(name, 'default') FROM users;", t))};
        CHECK(coal1.get_value().as<std::string_view>() == "Alice");

        auto coal2{UNWRAP(ctx.evaluate("SELECT COALESCE(age, 100) FROM users;", t))};
        CHECK(coal2.get_value().as<i32>() == 100);

        auto coal3{UNWRAP(
            ctx.evaluate("SELECT COALESCE(age, age, CAST(NULL AS INTEGER)) FROM users;", t))};
        CHECK(coal3.is_null());
    }

    SECTION("NULLIF") {
        auto nif1{UNWRAP(ctx.evaluate("SELECT NULLIF(id, 10) FROM users;", t))};
        CHECK(nif1.is_null());

        auto nif2{UNWRAP(ctx.evaluate("SELECT NULLIF(id, 20) FROM users;", t))};
        CHECK(nif2.get_value().as<i32>() == 10);
    }

    SECTION("String Functions (LOWER, UPPER, LENGTH, SUBSTR)") {
        auto lower{UNWRAP(ctx.evaluate("SELECT LOWER(name) FROM users;", t))};
        CHECK(lower.get_value().as<std::string_view>() == "alice");

        auto upper{UNWRAP(ctx.evaluate("SELECT UPPER(name) FROM users;", t))};
        CHECK(upper.get_value().as<std::string_view>() == "ALICE");

        auto len{UNWRAP(ctx.evaluate("SELECT LENGTH(name) FROM users;", t))};
        CHECK(len.get_value().as<i64>() == 5);

        auto sub1{UNWRAP(ctx.evaluate("SELECT SUBSTR(name, 1, 3) FROM users;", t))};
        CHECK(sub1.get_value().as<std::string_view>() == "Ali");

        auto sub2{UNWRAP(ctx.evaluate("SELECT SUBSTR(name, 2, 10) FROM users;", t))};
        CHECK(sub2.get_value().as<std::string_view>() == "lice");
    }

    SECTION("Math Functions (ABS, MOD)") {
        auto abs_pos{UNWRAP(ctx.evaluate("SELECT ABS(id) FROM users;", t))};
        CHECK(abs_pos.get_value().as<i32>() == 10);

        auto abs_neg{UNWRAP(ctx.evaluate("SELECT ABS(-id) FROM users;", t))};
        CHECK(abs_neg.get_value().as<i32>() == 10);

        auto mod_fn{UNWRAP(ctx.evaluate("SELECT MOD(id, 3) FROM users;", t))};
        CHECK(mod_fn.get_value().as<i32>() == 1);
    }
}

} // namespace cairn::tests
