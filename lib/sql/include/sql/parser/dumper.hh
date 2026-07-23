#pragma once

#include <ostream>

#include <magic_enum/magic_enum.hpp>
#include <stdx/fixed/string.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "sql/parser/parser.hh"
#include "support/indent.hh"

namespace cairn::sql::parser {

class dumper_t {
  public:
    dumper_t(const ast_t& ast, std::ostream& out) : ast_{ast}, out_{out} {}

    auto dump(node_id_t id) -> void {
        ast_[id].visit([&](const auto& node_data) { (*this)(node_data); });
    }

  private:
    auto operator()(stdx::monostate) -> void {}
    auto operator()(const literal_expr_t& node) -> void;
    auto operator()(const identifier_expr_t& node) -> void;
    auto operator()(const binary_expr_t& node) -> void;
    auto operator()(const aggregate_expr_t& node) -> void;
    auto operator()(const unary_expr_t& node) -> void;
    auto operator()(const function_expr_t& node) -> void;
    auto operator()(const select_stmt_t& node) -> void;
    auto operator()(const create_table_stmt_t& node) -> void;
    auto operator()(const drop_table_stmt_t& node) -> void;
    auto operator()(const alter_table_stmt_t& node) -> void;
    auto operator()(const create_index_stmt_t& node) -> void;
    auto operator()(const drop_index_stmt_t& node) -> void;

  private:
    const ast_t&      ast_;
    std::ostream&     out_;
    support::indent_t indent_;
};

} // namespace cairn::sql::parser
