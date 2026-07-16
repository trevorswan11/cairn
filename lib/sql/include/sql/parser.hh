#pragma once

#include <stdx/result.hh>
#include "sql/file.hh"
#include "sql/ast.hh"
#include "support/diagnostic/location.hh"

namespace cairn::sql {

using parse_result_t = stdx::result<ast::ast_t, location>;

[[nodiscard]] auto parse(const file& source_file) noexcept -> parse_result_t;

} // namespace cairn::sql
