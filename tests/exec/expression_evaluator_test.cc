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

    auto evaluate_and_check_string(std::string_view  query,
                                   const sql::tuple& t,
                                   std::string_view  expected) -> void {
        const sql::file f{query};
        auto            tree{UNWRAP(sql::parser::parse(f))};
        auto            bound_ast{UNWRAP(b.bind(tree, tree.roots()[0]))};
        const auto&     select{
            UNWRAP(bound_ast.get_as_opt<sql::binder::select_stmt_t>(bound_ast.roots()[0]))};
        auto bound_expr_id{select.select_list[0]};

        expression_evaluator_t evaluator{bound_ast, user_schema};
        auto                   res{UNWRAP(evaluator.evaluate(bound_expr_id, t))};
        CHECK_FALSE(res.is_null());
        CHECK(res.get_value().as<std::string_view>() == expected);
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
        CHECK(val.get_value().as<i32>() == 42);

        ctx.evaluate_and_check_string("SELECT 'Bob' FROM users;", t, "Bob");

        auto null_lit{UNWRAP(ctx.evaluate("SELECT NULL FROM users;", t))};
        CHECK(null_lit.is_null());
    }

    SECTION("Column references") {
        auto id_val{UNWRAP(ctx.evaluate("SELECT id FROM users;", t))};
        CHECK_FALSE(id_val.is_null());
        CHECK(id_val.get_value().as<i32>() == 10);

        ctx.evaluate_and_check_string("SELECT name FROM users;", t, "Alice");

        auto age_val{UNWRAP(ctx.evaluate("SELECT age FROM users;", t))};
        CHECK(age_val.is_null());
    }

    SECTION("Casts") {
        // Integer to Double implicit cast (id * 1.5)
        auto implicit_cast{UNWRAP(ctx.evaluate("SELECT id * 1.5 FROM users;", t))};
        CHECK_FALSE(implicit_cast.is_null());
        CHECK(implicit_cast.get_value().as<f64>() == 15.0);
        CHECK(implicit_cast.type() == sql::type::id_t::DOUBLE);

        // Null cast propagation (age * 1.5)
        auto null_cast{UNWRAP(ctx.evaluate("SELECT age * 1.5 FROM users;", t))};
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

        // Create a null-containing tuple: active = NULL
        std::vector<sql::value_t> values_null{sql::value_t{i32{10}, false},
                                              sql::value_t{std::string_view{"Alice"}},
                                              sql::value_t::make_null(sql::type::id_t::INTEGER),
                                              sql::value_t{f64{1250.50}},
                                              sql::value_t::make_null(sql::type::id_t::BOOLEAN)};
        sql::tuple t_null{UNWRAP(sql::tuple::serialize(ctx.user_schema, values_null))};

        // True AND Null -> Null
        auto t_and_null{UNWRAP(ctx.evaluate("SELECT (id = 10) AND active FROM users;", t_null))};
        CHECK(t_and_null.is_null());

        // False AND Null -> False
        auto f_and_null{UNWRAP(ctx.evaluate("SELECT (id = 5) AND active FROM users;", t_null))};
        CHECK(f_and_null.get_value().as<bool>() == false);

        // True OR Null -> True
        auto t_or_null{UNWRAP(ctx.evaluate("SELECT (id = 10) OR active FROM users;", t_null))};
        CHECK(t_or_null.get_value().as<bool>() == true);

        // False OR Null -> Null
        auto f_or_null{UNWRAP(ctx.evaluate("SELECT (id = 5) OR active FROM users;", t_null))};
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
        ctx.evaluate_and_check_string("SELECT COALESCE(name, 'default') FROM users;", t, "Alice");

        auto coal2{UNWRAP(ctx.evaluate("SELECT COALESCE(age, 100) FROM users;", t))};
        CHECK(coal2.get_value().as<i32>() == 100);

        auto coal3{UNWRAP(ctx.evaluate("SELECT COALESCE(age, age, age) FROM users;", t))};
        CHECK(coal3.is_null());
    }

    SECTION("NULLIF") {
        auto nif1{UNWRAP(ctx.evaluate("SELECT NULLIF(id, 10) FROM users;", t))};
        CHECK(nif1.is_null());

        auto nif2{UNWRAP(ctx.evaluate("SELECT NULLIF(id, 20) FROM users;", t))};
        CHECK(nif2.get_value().as<i32>() == 10);
    }

    SECTION("String Functions (LOWER, UPPER, LENGTH, SUBSTR)") {
        ctx.evaluate_and_check_string("SELECT LOWER(name) FROM users;", t, "alice");
        ctx.evaluate_and_check_string("SELECT UPPER(name) FROM users;", t, "ALICE");

        auto len{UNWRAP(ctx.evaluate("SELECT LENGTH(name) FROM users;", t))};
        CHECK(len.get_value().as<i64>() == 5);

        ctx.evaluate_and_check_string("SELECT SUBSTR(name, 1, 3) FROM users;", t, "Ali");
        ctx.evaluate_and_check_string("SELECT SUBSTR(name, 2, 10) FROM users;", t, "lice");
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
