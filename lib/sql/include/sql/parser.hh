#pragma once

#include "sql/ast.hh"
#include "sql/file.hh"
#include "support/diagnostic/error.hh"
#include "support/diagnostic/list.hh"

namespace cairn::sql {

[[nodiscard]] auto parse(const file& source_file, diagnostic_list& diags) noexcept
    -> result<ast::ast_t>;

} // namespace cairn::sql
