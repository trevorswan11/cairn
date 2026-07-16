#pragma once

#include "sql/ast.hh"
#include "sql/file.hh"
#include "support/diagnostic/location.hh"
#include <stdx/result.hh>

namespace cairn::sql {

using parse_result_t = stdx::result<ast::ast_t, location>;

[[nodiscard]] auto parse(const file& source_file) noexcept -> parse_result_t;

} // namespace cairn::sql
