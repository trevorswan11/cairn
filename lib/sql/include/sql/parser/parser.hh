#pragma once

#include <vector>

#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/fixed/string.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "sql/detail/ast.hh"
#include "sql/detail/node.hh"
#include "sql/file.hh"
#include "sql/type.hh"
#include "support/diagnostic/location.hh"

namespace cairn::sql::parser {

using detail::node_id_t;
using detail::node_kind_t;

using literal_value_t = stdx::variant<stdx::monostate, bool, i64, f64, stdx::fixed::string>;

struct literal_expr_t {
    literal_value_t value;
};

struct identifier_expr_t {
    stdx::fixed::string name;
};

enum class binary_op_t : u8 {
    ADD,
    SUB,
    MUL,
    DIV,
    MOD,
    EQ,
    NEQ,
    LT,
    GT,
    LTEQ,
    GTEQ,
    AND,
    OR,
};

[[nodiscard]] constexpr auto binary_op_to_string(binary_op_t op) noexcept -> std::string_view {
    switch (op) {
    case binary_op_t::ADD:  return "+";
    case binary_op_t::SUB:  return "-";
    case binary_op_t::MUL:  return "*";
    case binary_op_t::DIV:  return "/";
    case binary_op_t::MOD:  return "%";
    case binary_op_t::EQ:   return "=";
    case binary_op_t::NEQ:  return "!=";
    case binary_op_t::LT:   return "<";
    case binary_op_t::GT:   return ">";
    case binary_op_t::LTEQ: return "<=";
    case binary_op_t::GTEQ: return ">=";
    case binary_op_t::AND:  return "AND";
    case binary_op_t::OR:   return "OR";
    }
}
struct binary_expr_t {
    binary_op_t op;
    node_id_t   lhs;
    node_id_t   rhs;
};

struct select_item_t {
    stdx::option<node_id_t> expr;
};

enum class agg_func_t : u8 {
    COUNT,
    SUM,
    AVG,
    MIN,
    MAX,
};

struct aggregate_expr_t {
    agg_func_t              func;
    stdx::option<node_id_t> arg;
    bool                    is_distinct{false};
};

struct select_stmt_t {
    std::vector<select_item_t>        select_list;
    stdx::fixed::string               table_name;
    stdx::option<stdx::fixed::string> table_alias;
    stdx::option<node_id_t>           where_clause;
    std::vector<node_id_t>            group_by;
    stdx::option<node_id_t>           having_clause;
};

struct column_def_t {
    stdx::fixed::string      name;
    stdx::option<type::id_t> type;
    bool                     nullable{true};
};

struct create_table_stmt_t {
    stdx::fixed::string       table_name;
    std::vector<column_def_t> column_defs;
};

struct drop_table_stmt_t {
    stdx::fixed::string table_name;
};

struct alter_table_stmt_t {
    stdx::fixed::string table_name;
    column_def_t        column_def;
};

struct create_index_stmt_t {
    stdx::fixed::string              index_name;
    stdx::fixed::string              table_name;
    std::vector<stdx::fixed::string> columns;
};

struct drop_index_stmt_t {
    stdx::fixed::string index_name;
    stdx::fixed::string table_name;
};

enum class unary_op_t : u8 {
    MINUS,
    NOT,
    IS_NULL,
    IS_NOT_NULL,
};

struct unary_expr_t {
    unary_op_t op;
    node_id_t  expr;
};

struct function_expr_t {
    stdx::fixed::string    name;
    std::vector<node_id_t> args;
};

using node_data_t = stdx::variant<stdx::monostate,
                                  literal_expr_t,
                                  identifier_expr_t,
                                  binary_expr_t,
                                  aggregate_expr_t,
                                  unary_expr_t,
                                  function_expr_t,
                                  select_stmt_t,
                                  create_table_stmt_t,
                                  drop_table_stmt_t,
                                  alter_table_stmt_t,
                                  create_index_stmt_t,
                                  drop_index_stmt_t>;

using ast_t = detail::ast_t<node_data_t>;

[[nodiscard]] auto parse(const file& source_file) noexcept -> stdx::result<ast_t, location>;

template <stdx::StringLike S> auto parse(const S& input) -> stdx::result<ast_t, location> {
    const file f{stdx::string::to_view(input)};
    return parse(f);
}

} // namespace cairn::sql::parser
