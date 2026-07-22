#pragma once

#include <ostream>

#include "sql/parser/parser.hh"

namespace cairn::sql::parser {

// Dumps the entire AST structure recursively to the given output stream
auto dump_ast(const ast_t& ast, std::ostream& out) -> void;

} // namespace cairn::sql::parser
