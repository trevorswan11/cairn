#pragma once

#include <string>
#include <vector>

#include <stdx/assert.hh>
#include <stdx/result.hh>
#include <stdx/variant.hh>

#include "sql/binder/nodes.hh"
#include "sql/schema.hh"
#include "sql/tuple.hh"
#include "sql/type.hh"
#include "sql/value.hh"
#include "support/diagnostic/error.hh"

namespace cairn::exec {

#define EXPR_EVAL_NOOP(type)                                                \
    auto operator()(sql::binder::node_id_t, const type&, const sql::tuple&) \
        ->result<sql::value_t> {                                            \
        return stdx::err{error::SQL_UNEXPECTED_NODE};                       \
    }

class expression_evaluator_t {
  public:
    expression_evaluator_t(const sql::binder::ast_t& ast, const sql::schema& sch) noexcept
        : ast_{ast}, sch_{sch} {}

    [[nodiscard]] auto evaluate(sql::binder::node_id_t expr_id, const sql::tuple& input_tuple)
        -> result<sql::value_t> {
        return ast_[expr_id].visit(
            [&](const auto& node) { return (*this)(expr_id, node, input_tuple); });
    }

  private:
    [[nodiscard]] auto cast_value(const sql::value_t& val, sql::type::id_t target_type)
        -> result<sql::value_t>;

    [[nodiscard]] auto operator()(sql::binder::node_id_t id,
                                  const stdx::monostate& node,
                                  const sql::tuple&      input_tuple) -> result<sql::value_t>;
    [[nodiscard]] auto operator()(sql::binder::node_id_t             id,
                                  const sql::binder::literal_expr_t& node,
                                  const sql::tuple& input_tuple) -> result<sql::value_t>;
    [[nodiscard]] auto operator()(sql::binder::node_id_t                id,
                                  const sql::binder::column_ref_expr_t& node,
                                  const sql::tuple& input_tuple) -> result<sql::value_t>;
    [[nodiscard]] auto operator()(sql::binder::node_id_t            id,
                                  const sql::binder::binary_expr_t& node,
                                  const sql::tuple& input_tuple) -> result<sql::value_t>;
    [[nodiscard]] auto operator()(sql::binder::node_id_t          id,
                                  const sql::binder::cast_expr_t& node,
                                  const sql::tuple& input_tuple) -> result<sql::value_t>;
    [[nodiscard]] auto operator()(sql::binder::node_id_t           id,
                                  const sql::binder::unary_expr_t& node,
                                  const sql::tuple& input_tuple) -> result<sql::value_t>;
    [[nodiscard]] auto operator()(sql::binder::node_id_t              id,
                                  const sql::binder::function_expr_t& node,
                                  const sql::tuple& input_tuple) -> result<sql::value_t>;

    EXPR_EVAL_NOOP(sql::binder::aggregate_expr_t)
    EXPR_EVAL_NOOP(sql::binder::select_stmt_t)
    EXPR_EVAL_NOOP(sql::binder::create_table_stmt_t)
    EXPR_EVAL_NOOP(sql::binder::drop_table_stmt_t)
    EXPR_EVAL_NOOP(sql::binder::alter_table_stmt_t)
    EXPR_EVAL_NOOP(sql::binder::create_index_stmt_t)
    EXPR_EVAL_NOOP(sql::binder::drop_index_stmt_t)

  private:
    const sql::binder::ast_t& ast_;
    const sql::schema&        sch_;
    std::vector<std::string>  string_pool_;
};

#undef EXPR_EVAL_NOOP

} // namespace cairn::exec
