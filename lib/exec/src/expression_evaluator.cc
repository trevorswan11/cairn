#include "exec/expression_evaluator.hh"

#include <stdx/utility.hh>

#include "sql/binder/nodes.hh"
#include "sql/tuple.hh"
#include "sql/value.hh"
#include "stdx/variant.hh"
#include "support/diagnostic/error.hh"

namespace cairn::exec {

auto expression_evaluator_t::operator()(sql::binder::node_id_t id,
                                        const stdx::monostate& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t             id,
                                        const sql::binder::literal_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t                id,
                                        const sql::binder::column_ref_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t            id,
                                        const sql::binder::binary_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t          id,
                                        const sql::binder::cast_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t               id,
                                        const sql::binder::aggregate_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t           id,
                                        const sql::binder::unary_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t              id,
                                        const sql::binder::function_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t            id,
                                        const sql::binder::select_stmt_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t                  id,
                                        const sql::binder::create_table_stmt_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t                id,
                                        const sql::binder::drop_table_stmt_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t                 id,
                                        const sql::binder::alter_table_stmt_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t                  id,
                                        const sql::binder::create_index_stmt_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t                id,
                                        const sql::binder::drop_index_stmt_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

} // namespace cairn::exec
