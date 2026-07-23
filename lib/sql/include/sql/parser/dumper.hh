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
        ast_[id].visit([&](const auto& node_data) { this->visit(node_data); });
    }

  private:
    auto visit(stdx::monostate) -> void {}
    auto visit(const literal_expr_t& node) -> void;
    auto visit(const identifier_expr_t& node) -> void;
    auto visit(const binary_expr_t& node) -> void;
    auto visit(const aggregate_expr_t& node) -> void;
    auto visit(const select_stmt_t& node) -> void;
    auto visit(const create_table_stmt_t& node) -> void;
    auto visit(const drop_table_stmt_t& node) -> void;
    auto visit(const alter_table_stmt_t& node) -> void;
    auto visit(const create_index_stmt_t& node) -> void;
    auto visit(const drop_index_stmt_t& node) -> void;

  private:
    const ast_t&  ast_;
    std::ostream& out_;
    indent_t      indent_;
};

} // namespace cairn::sql::parser
