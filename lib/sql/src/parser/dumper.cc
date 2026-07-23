#include "sql/parser/dumper.hh"

#include <fmt/ostream.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/fixed/string.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "sql/parser/parser.hh"
#include "support/indent.hh"

namespace cairn::sql::parser {

using guard_t = support::indent_t::guard_t;

auto dumper_t::operator()(const literal_expr_t& node) -> void {
    fmt::print(out_, "LiteralExpr: ");
    node.value.visit([&](stdx::monostate) { fmt::println(out_, "NULL"); },
                     [&](bool val) { fmt::println(out_, "{}", val ? "TRUE" : "FALSE"); },
                     [&](i64 val) { fmt::println(out_, "{}", val); },
                     [&](f64 val) { fmt::println(out_, "{}", val); },
                     [&](const stdx::fixed::string& val) { fmt::println(out_, "'{}'", val); });
}

auto dumper_t::operator()(const identifier_expr_t& node) -> void {
    fmt::println(out_, "IdentifierExpr: {}", node.name);
}

auto dumper_t::operator()(const binary_expr_t& node) -> void {
    fmt::println(out_, "BinaryExpr ({})", binary_op_to_string(node.op));
    {
        guard_t g{indent_, false};
        fmt::print(out_, "{}LHS: ", indent_.current_branch());
        dump(node.lhs);
    }
    {
        guard_t g{indent_, true};
        fmt::print(out_, "{}RHS: ", indent_.current_branch());
        dump(node.rhs);
    }
}

auto dumper_t::operator()(const aggregate_expr_t& node) -> void {
    fmt::println(out_,
                 "AggregateExpr ({}){}",
                 magic_enum::enum_name(node.func),
                 node.is_distinct ? " (DISTINCT)" : "");

    guard_t g{indent_, true};
    if (node.arg) {
        fmt::print(out_, "{}Arg: ", indent_.current_branch());
        dump(*node.arg);
    } else {
        fmt::println(out_, "{}Arg: *", indent_.current_branch());
    }
}

auto dumper_t::operator()(const unary_expr_t& node) -> void {
    fmt::println(out_, "UnaryExpr ({})", magic_enum::enum_name(node.op));
    guard_t g{indent_, true};
    fmt::print(out_, "{}Expr: ", indent_.current_branch());
    dump(node.expr);
}

auto dumper_t::operator()(const function_expr_t& node) -> void {
    fmt::println(out_, "FunctionExpr ({})", node.name);
    if (!node.args.empty()) {
        guard_t g{indent_, true};
        fmt::println(out_, "{}Args:", indent_.current_branch());
        for (usize i{0}; i < node.args.size(); ++i) {
            guard_t g_item{indent_, i == node.args.size() - 1};
            fmt::print(out_, "{}", indent_.current_branch());
            dump(node.args[i]);
        }
    }
}

auto dumper_t::operator()(const select_stmt_t& node) -> void {
    fmt::println(out_, "SelectStmt");

    const auto has_where{node.where_clause.has_value()};
    const auto has_group_by{!node.group_by.empty()};
    const auto has_having{node.having_clause.has_value()};

    // Select List
    {
        guard_t g{indent_, false};
        fmt::println(out_, "{}Select List:", indent_.current_branch());
        for (usize i{0}; i < node.select_list.size(); ++i) {
            guard_t g_item{indent_, i == node.select_list.size() - 1};
            fmt::print(out_, "{}", indent_.current_branch());
            if (node.select_list[i].expr) {
                dump(*node.select_list[i].expr);
            } else {
                fmt::println(out_, "*");
            }
        }
    }

    // From Table
    {
        guard_t g{indent_, !has_where && !has_group_by && !has_having};
        fmt::print(out_, "{}From Table: {}", indent_.current_branch(), node.table_name);
        if (node.table_alias) { fmt::print(out_, " (Alias: {})", *node.table_alias); }
        fmt::println(out_, "");
    }

    // Where Clause
    if (has_where) {
        guard_t g{indent_, !has_group_by && !has_having};
        fmt::print(out_, "{}Where: ", indent_.current_branch());
        dump(*node.where_clause);
    }

    // Group By
    if (has_group_by) {
        guard_t g{indent_, !has_having};
        fmt::println(out_, "{}Group By:", indent_.current_branch());
        for (usize i{0}; i < node.group_by.size(); ++i) {
            guard_t g_item{indent_, i == node.group_by.size() - 1};
            fmt::print(out_, "{}", indent_.current_branch());
            dump(node.group_by[i]);
        }
    }

    // Having Clause
    if (has_having) {
        guard_t g{indent_, true};
        fmt::print(out_, "{}Having: ", indent_.current_branch());
        dump(*node.having_clause);
    }
}

auto dumper_t::operator()(const create_table_stmt_t& node) -> void {
    fmt::println(out_, "CreateTableStmt: {}", node.table_name);
    if (!node.column_defs.empty()) {
        guard_t g{indent_, true};
        fmt::println(out_, "{}ColumnDefs:", indent_.current_branch());
        for (usize i{0}; i < node.column_defs.size(); ++i) {
            guard_t g_item{indent_, i == node.column_defs.size() - 1};
            fmt::print(out_, "{}{} ", indent_.current_branch(), node.column_defs[i].name);
            if (node.column_defs[i].type) {
                fmt::print(out_, "{}", magic_enum::enum_name(*node.column_defs[i].type));
            } else {
                fmt::print(out_, "UNKNOWN");
            }
            fmt::println(out_, "{}", node.column_defs[i].nullable ? " NULL" : " NOT NULL");
        }
    }
}

auto dumper_t::operator()(const drop_table_stmt_t& node) -> void {
    fmt::println(out_, "DropTableStmt: {}", node.table_name);
}

auto dumper_t::operator()(const alter_table_stmt_t& node) -> void {
    guard_t g{indent_, true};
    fmt::print(out_, "AlterTableStmt: {}\n{}", node.table_name, indent_.current_branch());
    if (node.column_def.type) {
        fmt::println(out_,
                     "ADD COLUMN: {} {} {}",
                     node.column_def.name,
                     magic_enum::enum_name(*node.column_def.type),
                     node.column_def.nullable ? "NULL" : "NOT NULL");
    } else {
        fmt::println(out_, "DROP COLUMN: {}", node.column_def.name);
    }
}

auto dumper_t::operator()(const create_index_stmt_t& node) -> void {
    fmt::println(out_, "CreateIndexStmt: {} on {}", node.index_name, node.table_name);
    if (!node.columns.empty()) {
        guard_t g{indent_, true};
        fmt::println(out_, "{}Columns:", indent_.current_branch());
        for (usize i{0}; i < node.columns.size(); ++i) {
            guard_t g_item{indent_, i == node.columns.size() - 1};
            fmt::println(out_, "{}{}", indent_.current_branch(), node.columns[i]);
        }
    }
}

auto dumper_t::operator()(const drop_index_stmt_t& node) -> void {
    fmt::println(out_, "DropIndexStmt: {} on {}", node.index_name, node.table_name);
}

} // namespace cairn::sql::parser
