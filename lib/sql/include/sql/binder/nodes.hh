#pragma once

#include <vector>

#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/fixed/string.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "sql/catalog.hh"
#include "sql/detail/ast.hh"
#include "sql/detail/node.hh"
#include "sql/parser/parser.hh"
#include "sql/type.hh"

namespace cairn::sql::binder {

using detail::node_id_t;
using detail::node_kind_t;

struct literal_expr_t {
    parser::literal_value_t  value;
    stdx::option<type::id_t> type;
};

struct column_ref_expr_t {
    table_id_t               table_id;
    usize                    column_idx;
    stdx::fixed::string      column_name;
    stdx::option<type::id_t> type;
};

struct binary_expr_t {
    parser::binary_op_t      op;
    node_id_t                lhs;
    node_id_t                rhs;
    stdx::option<type::id_t> type;
};

struct cast_expr_t {
    node_id_t  expr;
    type::id_t target_type;
};

struct aggregate_expr_t {
    parser::agg_func_t       func;
    stdx::option<node_id_t>  arg;
    bool                     is_distinct{false};
    stdx::option<type::id_t> return_type;
};

struct select_stmt_t {
    table_id_t                        table_id;
    stdx::fixed::string               table_name;
    stdx::option<stdx::fixed::string> table_alias;
    std::vector<node_id_t>            select_list;
    stdx::option<node_id_t>           where_clause;
    std::vector<node_id_t>            group_by;
    stdx::option<node_id_t>           having_clause;
};

struct create_table_stmt_t {
    stdx::fixed::string               table_name;
    std::vector<parser::column_def_t> column_defs;
};

struct drop_table_stmt_t {
    table_id_t          table_id;
    stdx::fixed::string table_name;
};

struct alter_table_stmt_t {
    table_id_t           table_id;
    stdx::fixed::string  table_name;
    parser::column_def_t column_def;
};

struct create_index_stmt_t {
    stdx::fixed::string              index_name;
    table_id_t                       table_id;
    stdx::fixed::string              table_name;
    std::vector<stdx::fixed::string> column_names;
    std::vector<usize>               column_indices;
};

struct drop_index_stmt_t {
    stdx::fixed::string index_name;
    table_id_t          table_id;
    stdx::fixed::string table_name;
};

using node_data_t = stdx::variant<stdx::monostate,
                                  literal_expr_t,
                                  column_ref_expr_t,
                                  binary_expr_t,
                                  cast_expr_t,
                                  aggregate_expr_t,
                                  select_stmt_t,
                                  create_table_stmt_t,
                                  drop_table_stmt_t,
                                  alter_table_stmt_t,
                                  create_index_stmt_t,
                                  drop_index_stmt_t>;

using ast_t = detail::ast_t<node_data_t>;

} // namespace cairn::sql::binder
