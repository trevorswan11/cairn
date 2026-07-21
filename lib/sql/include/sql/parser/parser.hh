#pragma once

#include <vector>

#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/fixed/string.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
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
    SUBTRACT,
    MULTIPLY,
    DIVIDE,
    EQUAL,
    NOT_EQUAL,
    LESS_THAN,
    GREATER_THAN,
    LESS_THAN_OR_EQUAL,
    GREATER_THAN_OR_EQUAL,
    AND,
    OR,
};

struct binary_expr_t {
    binary_op_t op;
    node_id_t   lhs;
    node_id_t   rhs;
};

struct select_item_t {
    stdx::option<node_id_t> expr;
};

struct select_stmt_t {
    std::vector<select_item_t>        select_list;
    stdx::fixed::string               table_name;
    stdx::option<stdx::fixed::string> table_alias;
    stdx::option<node_id_t>           where_clause;
};

struct column_def_t {
    stdx::fixed::string name;
    type::id_t          type{type::id_t::INVALID};
    bool                nullable{true};
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

using node_data_t = stdx::variant<stdx::monostate,
                                  literal_expr_t,
                                  identifier_expr_t,
                                  binary_expr_t,
                                  select_stmt_t,
                                  create_table_stmt_t,
                                  drop_table_stmt_t,
                                  alter_table_stmt_t,
                                  create_index_stmt_t,
                                  drop_index_stmt_t>;

using ast_t = detail::ast_t<node_data_t>;

[[nodiscard]] auto parse(const file& source_file) noexcept -> stdx::result<ast_t, location>;

} // namespace cairn::sql::parser
