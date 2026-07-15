#pragma once

#include <vector>
#include <utility>

#include "sql/file.hh"
#include "sql/ast.hh"
#include "support/diagnostic/location.hh"
#include "support/diagnostic/error.hh"

namespace cairn::sql {

using parse_diags_t = std::vector<location>;
using parse_result_t = std::pair<result<ast::ast_t>, parse_diags_t>;

[[nodiscard]] auto parse(const file& source_file) noexcept -> parse_result_t;

} // namespace cairn::sql
