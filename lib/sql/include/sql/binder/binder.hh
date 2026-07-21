#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/pointers>
#include <stdx/fixed/string.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "sql/binder/nodes.hh"
#include "sql/catalog.hh"
#include "sql/detail/node.hh"
#include "sql/parser/parser.hh"
#include "sql/schema.hh"
#include "sql/type.hh"
#include "support/diagnostic/error.hh"
#include "support/diagnostic/location.hh"

namespace cairn::sql::binder {

template <usize PoolSize> class binder_t {
  public:
    struct binding_scope_t {
        stdx::fixed::string          name;
        table_id_t                   table_id;
        gsl::not_null<const schema*> table_schema;
    };

  public:
    explicit binder_t(const catalog<PoolSize>& cat) noexcept : catalog_{cat} {}

    [[nodiscard]] auto bind(const parser::ast_t& tree, node_id_t root_id)
        -> stdx::result<ast_t, diagnostic> {
        if (!root_id.is_valid()) { return stdx::err{diagnostic{error::IO_ERROR}}; }
        const auto& node_data{tree[root_id]};
        const auto  loc{tree.get_location(root_id)};

        ast_t ast;
        TRY(node_data.visit(
            [&](stdx::monostate) -> stdx::result<void, diagnostic> {
                return stdx::err{diagnostic{error::IO_ERROR, loc}};
            },
            [&](const parser::select_stmt_t& select) -> stdx::result<void, diagnostic> {
                ast.add_root(TRY(bind_select(tree, root_id, select, ast)));
                return {};
            },
            [&](const parser::create_table_stmt_t& create) -> stdx::result<void, diagnostic> {
                auto tbl_name{create.table_name};
                if (tbl_name.empty()) {
                    return stdx::err{diagnostic{error::SQL_TABLE_NOT_FOUND, loc}};
                }

                ast.add_root(
                    ast.template add_node<create_table_stmt_t>(loc, tbl_name, create.column_defs));
                return {};
            },
            [&](const parser::drop_table_stmt_t& drop) -> stdx::result<void, diagnostic> {
                auto tbl_name{drop.table_name};
                auto tbl{catalog_.get_table(tbl_name.view())};
                if (!tbl) { return stdx::err{diagnostic{error::SQL_TABLE_NOT_FOUND, loc}}; }

                ast.add_root(
                    ast.template add_node<drop_table_stmt_t>(loc, tbl->table_id, tbl_name));
                return {};
            },
            [&](const parser::alter_table_stmt_t& alter) -> stdx::result<void, diagnostic> {
                auto tbl_name{alter.table_name};
                auto tbl{catalog_.get_table(tbl_name.view())};
                if (!tbl) { return stdx::err{diagnostic{error::SQL_TABLE_NOT_FOUND, loc}}; }

                ast.add_root(ast.template add_node<alter_table_stmt_t>(
                    loc, tbl->table_id, tbl_name, alter.column_def));
                return {};
            },
            [&](const parser::create_index_stmt_t& idx) -> stdx::result<void, diagnostic> {
                auto tbl_name{idx.table_name};
                auto tbl{catalog_.get_table(tbl_name.view())};
                if (!tbl) { return stdx::err{diagnostic{error::SQL_TABLE_NOT_FOUND, loc}}; }

                std::vector<stdx::fixed::string> col_names;
                std::vector<usize>               col_indices;
                for (const auto& col_name_fixed : idx.columns) {
                    auto       col_name{col_name_fixed.view()};
                    const auto columns{tbl->table_schema.columns()};
                    const auto it{std::ranges::find_if(
                        columns, [&](const auto& col) { return col.name() == col_name; })};
                    if (it == columns.end()) {
                        return stdx::err{diagnostic{error::SQL_COLUMN_NOT_FOUND, loc}};
                    }

                    col_names.emplace_back(col_name_fixed);
                    col_indices.emplace_back(
                        static_cast<usize>(std::distance(columns.begin(), it)));
                }

                ast.add_root(ast.template add_node<create_index_stmt_t>(loc,
                                                                        idx.index_name,
                                                                        tbl->table_id,
                                                                        tbl_name,
                                                                        std::move(col_names),
                                                                        std::move(col_indices)));
                return {};
            },
            [&](const parser::drop_index_stmt_t& drop_idx) -> stdx::result<void, diagnostic> {
                stdx::fixed::string tbl_name{drop_idx.table_name};
                auto                tbl{catalog_.get_table(tbl_name.view())};
                if (!tbl) { return stdx::err{diagnostic{error::SQL_TABLE_NOT_FOUND, loc}}; }

                ast.add_root(ast.template add_node<drop_index_stmt_t>(
                    loc, drop_idx.index_name, tbl->table_id, tbl_name));
                return {};
            },
            [&](const auto&) -> stdx::result<void, diagnostic> {
                return stdx::err{diagnostic{error::IO_ERROR, loc}};
            }));
        return ast;
    }

  private:
    [[nodiscard]] auto bind_select(const parser::ast_t&         tree,
                                   node_id_t                    root_id,
                                   const parser::select_stmt_t& stmt,
                                   ast_t& ast) -> stdx::result<node_id_t, diagnostic> {
        const auto loc{tree.get_location(root_id)};

        stdx::fixed::string tbl_name{stmt.table_name};
        auto                tbl{catalog_.get_table(tbl_name.view())};
        if (!tbl) { return stdx::err{diagnostic{error::SQL_TABLE_NOT_FOUND, loc}}; }
        auto tbl_alias{stmt.table_alias};

        std::vector<binding_scope_t> scopes;
        scopes.emplace_back(
            binding_scope_t{tbl_alias.value_or(tbl_name), tbl->table_id, &tbl->table_schema});

        std::vector<node_id_t> bound_select_list;
        for (const auto& item : stmt.select_list) {
            if (!item.expr) {
                for (usize i{0}; i < tbl->table_schema.column_count(); ++i) {
                    const auto& col{tbl->table_schema[i]};
                    bound_select_list.emplace_back(ast.template add_node<column_ref_expr_t>(
                        loc, tbl->table_id, i, stdx::fixed::string{col.name()}, col.type()));
                }
            } else {
                bound_select_list.emplace_back(TRY(bind_expression(tree, *item.expr, scopes, ast)));
            }
        }

        stdx::option<node_id_t> bound_where_clause;
        if (stmt.where_clause) {
            auto bound_expr_id{TRY(bind_expression(tree, *stmt.where_clause, scopes, ast))};
            if (get_expr_type(ast, bound_expr_id) != type::id_t::BOOLEAN) {
                const auto expr_loc{tree.get_location(*stmt.where_clause)};
                return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, expr_loc}};
            }
            bound_where_clause.emplace(bound_expr_id);
        }

        return ast.template add_node<select_stmt_t>(loc,
                                                    tbl->table_id,
                                                    std::move(tbl_name),
                                                    std::move(tbl_alias),
                                                    std::move(bound_select_list),
                                                    bound_where_clause);
    }

    [[nodiscard]] auto bind_expression(const parser::ast_t&                tree,
                                       node_id_t                           expr_id,
                                       const std::vector<binding_scope_t>& scopes,
                                       ast_t& ast) -> stdx::result<node_id_t, diagnostic> {
        const auto  loc{tree.get_location(expr_id)};
        const auto& node_data{tree[expr_id]};

        return node_data.visit(
            [&](stdx::monostate) -> stdx::result<node_id_t, diagnostic> {
                return stdx::err{diagnostic{error::IO_ERROR, loc}};
            },
            [&](const parser::literal_expr_t& lit) -> stdx::result<node_id_t, diagnostic> {
                type::id_t lit_type{type::id_t::INVALID};
                if (lit.value.template is<bool>()) {
                    lit_type = type::id_t::BOOLEAN;
                } else if (lit.value.template is<i64>()) {
                    lit_type = type::id_t::INTEGER;
                } else if (lit.value.template is<f64>()) {
                    lit_type = type::id_t::DOUBLE;
                } else if (lit.value.template is<stdx::fixed::string>()) {
                    lit_type = type::id_t::VARCHAR;
                } else if (lit.value.template is<stdx::monostate>()) {
                    lit_type = type::id_t::INVALID;
                }
                return ast.template add_node<literal_expr_t>(loc, lit.value, lit_type);
            },
            [&](const parser::identifier_expr_t& ident) -> stdx::result<node_id_t, diagnostic> {
                std::string_view full_name{ident.name.view()};
                std::string_view prefix, col_name{full_name};
                const auto       dot_pos{full_name.find('.')};
                if (dot_pos != std::string_view::npos) {
                    prefix   = stdx::string::substr(full_name, 0, dot_pos);
                    col_name = stdx::string::substr(full_name, dot_pos + 1);
                }

                if (!prefix.empty()) {
                    const auto scope_it{
                        std::find_if(scopes.begin(), scopes.end(), [&](const auto& s) {
                            return s.name.view() == prefix;
                        })};
                    if (scope_it == scopes.end()) {
                        return stdx::err{diagnostic{error::SQL_TABLE_NOT_FOUND, loc}};
                    }
                    const auto* matched_scope{&(*scope_it)};

                    const auto columns{matched_scope->table_schema->columns()};
                    const auto col_it{
                        std::find_if(columns.begin(), columns.end(), [&](const auto& col) {
                            return col.name() == col_name;
                        })};
                    if (col_it == columns.end()) {
                        return stdx::err{
                            diagnostic{error::SQL_COLUMN_NOT_FOUND, loc.line, loc.column}};
                    }

                    const auto  col_idx{static_cast<usize>(std::distance(columns.begin(), col_it))};
                    const auto& col{(*matched_scope->table_schema)[col_idx]};
                    return ast.template add_node<column_ref_expr_t>(loc,
                                                                    matched_scope->table_id,
                                                                    col_idx,
                                                                    stdx::fixed::string{col_name},
                                                                    col.type());
                } else {
                    stdx::option<const binding_scope_t&> matched_scope;
                    stdx::option<usize>                  matched_col_idx;
                    usize                                match_count{0};

                    for (const auto& scope : scopes) {
                        const auto columns{scope.table_schema->columns()};
                        const auto it{
                            std::find_if(columns.begin(), columns.end(), [&](const auto& col) {
                                return col.name() == col_name;
                            })};
                        if (it != columns.end()) {
                            matched_scope.emplace(scope);
                            matched_col_idx.emplace(std::distance(columns.begin(), it));
                            match_count++;
                        }
                    }

                    if (match_count > 1) {
                        return stdx::err{
                            diagnostic{error::SQL_COLUMN_AMBIGUOUS, loc.line, loc.column}};
                    }
                    if (match_count == 1) {
                        const auto& col{(*matched_scope->table_schema)[*matched_col_idx]};
                        return ast.template add_node<column_ref_expr_t>(
                            loc,
                            matched_scope->table_id,
                            *matched_col_idx,
                            stdx::fixed::string{col_name},
                            col.type());
                    }
                    return stdx::err{diagnostic{error::SQL_COLUMN_NOT_FOUND, loc.line, loc.column}};
                }
            },
            [&](const parser::binary_expr_t& binary) -> stdx::result<node_id_t, diagnostic> {
                auto bound_lhs_id{TRY(bind_expression(tree, binary.lhs, scopes, ast))};
                auto bound_rhs_id{TRY(bind_expression(tree, binary.rhs, scopes, ast))};

                const auto l_type{get_expr_type(ast, bound_lhs_id)};
                const auto r_type{get_expr_type(ast, bound_rhs_id)};
                type::id_t res_type{type::id_t::INVALID};

                switch (binary.op) {
                case parser::binary_op_t::AND:
                case parser::binary_op_t::OR:  {
                    if (l_type != type::id_t::BOOLEAN || r_type != type::id_t::BOOLEAN) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    res_type = type::id_t::BOOLEAN;
                    break;
                }
                case parser::binary_op_t::EQUAL:
                case parser::binary_op_t::NOT_EQUAL:
                case parser::binary_op_t::LESS_THAN:
                case parser::binary_op_t::LESS_THAN_OR_EQUAL:
                case parser::binary_op_t::GREATER_THAN:
                case parser::binary_op_t::GREATER_THAN_OR_EQUAL: {
                    bool compatible{false};
                    if (l_type == r_type) {
                        compatible = true;
                    } else if (type::is_numeric(l_type) && type::is_numeric(r_type)) {
                        compatible = true;
                    }
                    if (!compatible) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    res_type = type::id_t::BOOLEAN;
                    break;
                }
                case parser::binary_op_t::ADD:
                case parser::binary_op_t::SUBTRACT:
                case parser::binary_op_t::MULTIPLY:
                case parser::binary_op_t::DIVIDE:   {
                    if (!type::is_numeric(l_type) || !type::is_numeric(r_type)) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    if (l_type == type::id_t::DOUBLE || r_type == type::id_t::DOUBLE) {
                        res_type = type::id_t::DOUBLE;
                    } else if (l_type == type::id_t::FLOAT || r_type == type::id_t::FLOAT) {
                        res_type = type::id_t::FLOAT;
                    } else if (l_type == type::id_t::BIGINT || r_type == type::id_t::BIGINT) {
                        res_type = type::id_t::BIGINT;
                    } else if (l_type == type::id_t::INTEGER || r_type == type::id_t::INTEGER) {
                        res_type = type::id_t::INTEGER;
                    } else if (l_type == type::id_t::SMALLINT || r_type == type::id_t::SMALLINT) {
                        res_type = type::id_t::SMALLINT;
                    } else {
                        res_type = type::id_t::TINYINT;
                    }
                    break;
                }
                }

                return ast.template add_node<binary_expr_t>(
                    loc, binary.op, bound_lhs_id, bound_rhs_id, res_type);
            },
            [&](const auto&) -> stdx::result<node_id_t, diagnostic> {
                return stdx::err{diagnostic{error::IO_ERROR, loc}};
            });
    }

    [[nodiscard]] auto get_expr_type(const ast_t& ast, node_id_t id) const noexcept -> type::id_t {
        return ast[id].visit([](const literal_expr_t& lit) { return lit.type; },
                             [](const column_ref_expr_t& col) { return col.type; },
                             [](const binary_expr_t& bin) { return bin.type; },
                             [](const auto&) { return type::id_t::INVALID; });
    }

  private:
    const catalog<PoolSize>& catalog_;
};

} // namespace cairn::sql::binder
