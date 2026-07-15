#pragma once

#include <stdx/memory.hh>

#include "sql/ast.hh"
#include "sql/file.hh"
#include "support/diagnostic/error.hh"
#include "support/diagnostic/list.hh"

namespace cairn::sql {

[[nodiscard]] auto parse(const file& source_file, diagnostic_list& diags) noexcept
    -> result<stdx::box<ast::stmt_node_t>>;

} // namespace cairn::sql
