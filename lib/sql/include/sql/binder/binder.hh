#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
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
#include "support/string_utils.hh"

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

        ankerl::unordered_dense::set<std::string_view, string_utils::ihash, string_utils::iequals>
            seen_col_names;

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

                seen_col_names.clear();
                seen_col_names.reserve(create.column_defs.size());
                for (const auto& col_def : create.column_defs) {
                    if (!seen_col_names.insert(col_def.name.view()).second) {
                        return stdx::err{diagnostic{error::SQL_CONSTRAINT_VIOLATION, loc}};
                    }
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

                const auto columns{tbl->table_schema.columns()};
                const auto it{std::ranges::find_if(columns, [&](const auto& col) {
                    return col.name() == alter.column_def.name.view();
                })};

                if (alter.column_def.type.has_value()) {
                    // ADD COLUMN: Check that column doesn't exist already
                    if (it != columns.end()) {
                        return stdx::err{diagnostic{error::SQL_CONSTRAINT_VIOLATION, loc}};
                    }
                } else {
                    // DROP COLUMN: Check that column does exist
                    if (it == columns.end()) {
                        return stdx::err{diagnostic{error::SQL_COLUMN_NOT_FOUND, loc}};
                    }
                }

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
            if (contains_aggregate(ast, bound_expr_id)) {
                const auto expr_loc{tree.get_location(*stmt.where_clause)};
                return stdx::err{diagnostic{error::SQL_INVALID_AGGREGATE, expr_loc}};
            }
            if (get_expr_type(ast, bound_expr_id) != type::id_t::BOOLEAN) {
                const auto expr_loc{tree.get_location(*stmt.where_clause)};
                return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, expr_loc}};
            }
            bound_where_clause.emplace(bound_expr_id);
        }

        std::vector<node_id_t> bound_group_by;
        for (const auto& g_expr_id : stmt.group_by) {
            bound_group_by.emplace_back(TRY(bind_expression(tree, g_expr_id, scopes, ast)));
        }

        stdx::option<node_id_t> bound_having_clause;
        if (stmt.having_clause) {
            auto bound_expr_id{TRY(bind_expression(tree, *stmt.having_clause, scopes, ast))};
            if (get_expr_type(ast, bound_expr_id) != type::id_t::BOOLEAN) {
                const auto expr_loc{tree.get_location(*stmt.having_clause)};
                return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, expr_loc}};
            }
            bound_having_clause.emplace(bound_expr_id);
        }

        // Grouping validation
        const bool has_aggregates{
            std::ranges::any_of(bound_select_list,
                                [&](node_id_t id) { return contains_aggregate(ast, id); }) ||
            (bound_having_clause && contains_aggregate(ast, *bound_having_clause))};

        if (has_aggregates || !bound_group_by.empty()) {
            std::vector<column_ref_expr_t> grouped_cols;
            for (const auto g_id : bound_group_by) {
                collect_unaggregated_cols(ast, g_id, grouped_cols);
            }

            const auto is_col_grouped = [&](const column_ref_expr_t& col) {
                return std::ranges::any_of(grouped_cols, [&](const column_ref_expr_t& g_col) {
                    return g_col.table_id == col.table_id && g_col.column_idx == col.column_idx;
                });
            };

            std::vector<column_ref_expr_t> unagg_cols;
            for (const auto s_id : bound_select_list) {
                collect_unaggregated_cols(ast, s_id, unagg_cols);
            }
            if (bound_having_clause) {
                collect_unaggregated_cols(ast, *bound_having_clause, unagg_cols);
            }

            if (!std::ranges::all_of(unagg_cols,
                                     [&](const auto& col) { return is_col_grouped(col); })) {
                return stdx::err{diagnostic{error::SQL_UNGROUPED_COLUMN, loc}};
            }
        }

        return ast.template add_node<select_stmt_t>(loc,
                                                    tbl->table_id,
                                                    std::move(tbl_name),
                                                    std::move(tbl_alias),
                                                    std::move(bound_select_list),
                                                    bound_where_clause,
                                                    std::move(bound_group_by),
                                                    bound_having_clause);
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
                stdx::option<type::id_t> lit_type;
                if (lit.value.template is<bool>()) {
                    lit_type.emplace(type::id_t::BOOLEAN);
                } else if (lit.value.template is<i64>()) {
                    lit_type.emplace(type::id_t::INTEGER);
                } else if (lit.value.template is<f64>()) {
                    lit_type.emplace(type::id_t::DOUBLE);
                } else if (lit.value.template is<stdx::fixed::string>()) {
                    lit_type.emplace(type::id_t::VARCHAR);
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
            [&](const parser::aggregate_expr_t& agg) -> stdx::result<node_id_t, diagnostic> {
                stdx::option<node_id_t>  bound_arg;
                stdx::option<type::id_t> arg_type;
                if (agg.arg) {
                    bound_arg.emplace(TRY(bind_expression(tree, *agg.arg, scopes, ast)));
                    arg_type = get_expr_type(ast, *bound_arg);
                }

                stdx::option<type::id_t> ret_type;
                switch (agg.func) {
                case parser::agg_func_t::COUNT: ret_type = type::id_t::BIGINT; break;
                case parser::agg_func_t::SUM:
                case parser::agg_func_t::AVG:
                    if (!type::is_numeric(arg_type)) {
                        return stdx::err{diagnostic{error::SQL_INVALID_AGGREGATE, loc}};
                    }

                    if (agg.func == parser::agg_func_t::AVG || arg_type == type::id_t::DOUBLE) {
                        ret_type.emplace(type::id_t::DOUBLE);
                    } else {
                        ret_type.emplace(type::id_t::BIGINT);
                    }
                    break;
                case parser::agg_func_t::MIN:
                case parser::agg_func_t::MAX:
                    if (!arg_type) {
                        return stdx::err{diagnostic{error::SQL_INVALID_AGGREGATE, loc}};
                    }
                    ret_type = arg_type;
                    break;
                }

                return ast.template add_node<aggregate_expr_t>(
                    loc, agg.func, bound_arg, agg.is_distinct, ret_type);
            },
            [&](const parser::binary_expr_t& binary) -> stdx::result<node_id_t, diagnostic> {
                auto bound_lhs_id{TRY(bind_expression(tree, binary.lhs, scopes, ast))};
                auto bound_rhs_id{TRY(bind_expression(tree, binary.rhs, scopes, ast))};

                const auto               l_type{get_expr_type(ast, bound_lhs_id)};
                const auto               r_type{get_expr_type(ast, bound_rhs_id)};
                stdx::option<type::id_t> res_type;

                switch (binary.op) {
                case parser::binary_op_t::AND:
                case parser::binary_op_t::OR:  {
                    if (l_type != type::id_t::BOOLEAN || r_type != type::id_t::BOOLEAN) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    res_type.emplace(type::id_t::BOOLEAN);
                    break;
                }
                case parser::binary_op_t::EQ:
                case parser::binary_op_t::NEQ:
                case parser::binary_op_t::LT:
                case parser::binary_op_t::LTEQ:
                case parser::binary_op_t::GT:
                case parser::binary_op_t::GTEQ: {
                    auto target_type_opt{type::common_type(l_type, r_type)};
                    if (!target_type_opt) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    const auto target_type{*target_type_opt};

                    if (l_type != target_type) {
                        if (!type::can_coerce(l_type, target_type)) {
                            return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                        }
                        bound_lhs_id =
                            ast.template add_node<cast_expr_t>(loc, bound_lhs_id, target_type);
                    }

                    if (r_type != target_type) {
                        if (!type::can_coerce(r_type, target_type)) {
                            return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                        }
                        bound_rhs_id =
                            ast.template add_node<cast_expr_t>(loc, bound_rhs_id, target_type);
                    }
                    res_type.emplace(type::id_t::BOOLEAN);
                    break;
                }
                case parser::binary_op_t::ADD:
                case parser::binary_op_t::SUB:
                case parser::binary_op_t::MUL:
                case parser::binary_op_t::DIV:
                case parser::binary_op_t::MOD: {
                    auto target_type_opt{type::common_type(l_type, r_type)};
                    if (!type::is_numeric(target_type_opt)) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    const auto target_type{*target_type_opt};

                    if (l_type != target_type) {
                        if (!type::can_coerce(l_type, target_type)) {
                            return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                        }
                        bound_lhs_id =
                            ast.template add_node<cast_expr_t>(loc, bound_lhs_id, target_type);
                    }

                    if (r_type != target_type) {
                        if (!type::can_coerce(r_type, target_type)) {
                            return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                        }
                        bound_rhs_id =
                            ast.template add_node<cast_expr_t>(loc, bound_rhs_id, target_type);
                    }
                    res_type.emplace(target_type);
                    break;
                }
                }

                return ast.template add_node<binary_expr_t>(
                    loc, binary.op, bound_lhs_id, bound_rhs_id, res_type);
            },
            [&](const parser::unary_expr_t& unary) -> stdx::result<node_id_t, diagnostic> {
                auto bound_expr_id{TRY(bind_expression(tree, unary.expr, scopes, ast))};
                auto expr_type{get_expr_type(ast, bound_expr_id)};

                stdx::option<type::id_t> res_type;
                unary_op_t               bound_op;

                switch (unary.op) {
                case parser::unary_op_t::MINUS: {
                    if (!type::is_numeric(expr_type)) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    res_type = expr_type;
                    bound_op = unary_op_t::MINUS;
                    break;
                }
                case parser::unary_op_t::NOT: {
                    if (expr_type != type::id_t::BOOLEAN) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    res_type.emplace(type::id_t::BOOLEAN);
                    bound_op = unary_op_t::NOT;
                    break;
                }
                case parser::unary_op_t::IS_NULL: {
                    res_type.emplace(type::id_t::BOOLEAN);
                    bound_op = unary_op_t::IS_NULL;
                    break;
                }
                case parser::unary_op_t::IS_NOT_NULL: {
                    res_type.emplace(type::id_t::BOOLEAN);
                    bound_op = unary_op_t::IS_NOT_NULL;
                    break;
                }
                }
                return ast.template add_node<unary_expr_t>(loc, bound_op, bound_expr_id, res_type);
            },
            [&](const parser::function_expr_t& func) -> stdx::result<node_id_t, diagnostic> {
                std::vector<node_id_t>                bound_args;
                std::vector<stdx::option<type::id_t>> arg_types;
                for (auto arg_id : func.args) {
                    auto bound_id{TRY(bind_expression(tree, arg_id, scopes, ast))};
                    bound_args.emplace_back(bound_id);
                    arg_types.emplace_back(get_expr_type(ast, bound_id));
                }

                func_type_t              bound_func;
                stdx::option<type::id_t> res_type;
                std::string_view         name_view{func.name.view()};

                if (string_utils::iequals{}(name_view, "COALESCE")) {
                    if (bound_args.empty()) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }

                    stdx::option<type::id_t> common{arg_types[0]};
                    for (usize i{1}; i < arg_types.size(); ++i) {
                        common = type::common_type(common, arg_types[i]);
                    }
                    if (!common) { return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}}; }

                    for (usize i{0}; i < bound_args.size(); ++i) {
                        if (arg_types[i] != common) {
                            if (!type::can_coerce(arg_types[i], common)) {
                                return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                            }
                            bound_args[i] =
                                ast.template add_node<cast_expr_t>(loc, bound_args[i], *common);
                        }
                    }
                    bound_func = func_type_t::COALESCE;
                    res_type   = common;
                } else if (string_utils::iequals{}(name_view, "NULLIF")) {
                    if (bound_args.size() != 2) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    auto common{type::common_type(arg_types[0], arg_types[1])};
                    if (!common) { return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}}; }
                    for (usize i{0}; i < 2; ++i) {
                        if (arg_types[i] != common) {
                            if (!type::can_coerce(arg_types[i], common)) {
                                return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                            }
                            bound_args[i] =
                                ast.template add_node<cast_expr_t>(loc, bound_args[i], *common);
                        }
                    }
                    bound_func = func_type_t::NULLIF;
                    res_type   = common;
                } else if (string_utils::iequals{}(name_view, "LOWER") ||
                           string_utils::iequals{}(name_view, "UPPER")) {
                    if (bound_args.size() != 1) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    if (arg_types[0] != type::id_t::VARCHAR) {
                        if (!type::can_coerce(arg_types[0], type::id_t::VARCHAR)) {
                            return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                        }
                        bound_args[0] = ast.template add_node<cast_expr_t>(
                            loc, bound_args[0], type::id_t::VARCHAR);
                    }
                    bound_func = string_utils::iequals{}(name_view, "LOWER") ? func_type_t::LOWER
                                                                             : func_type_t::UPPER;
                    res_type.emplace(type::id_t::VARCHAR);
                } else if (string_utils::iequals{}(name_view, "LENGTH")) {
                    if (bound_args.size() != 1) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    if (arg_types[0] != type::id_t::VARCHAR) {
                        if (!type::can_coerce(arg_types[0], type::id_t::VARCHAR)) {
                            return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                        }
                        bound_args[0] = ast.template add_node<cast_expr_t>(
                            loc, bound_args[0], type::id_t::VARCHAR);
                    }
                    bound_func = func_type_t::LENGTH;
                    res_type.emplace(type::id_t::BIGINT);
                } else if (string_utils::iequals{}(name_view, "SUBSTR")) {
                    if (bound_args.size() != 3) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    if (arg_types[0] != type::id_t::VARCHAR) {
                        if (!type::can_coerce(arg_types[0], type::id_t::VARCHAR)) {
                            return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                        }
                        bound_args[0] = ast.template add_node<cast_expr_t>(
                            loc, bound_args[0], type::id_t::VARCHAR);
                    }
                    for (usize i{1}; i < 3; ++i) {
                        if (!type::is_numeric(arg_types[i])) {
                            return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                        }
                        if (arg_types[i] != type::id_t::INTEGER) {
                            if (!type::can_coerce(arg_types[i], type::id_t::INTEGER)) {
                                return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                            }
                            bound_args[i] = ast.template add_node<cast_expr_t>(
                                loc, bound_args[i], type::id_t::INTEGER);
                        }
                    }
                    bound_func = func_type_t::SUBSTR;
                    res_type.emplace(type::id_t::VARCHAR);
                } else if (string_utils::iequals{}(name_view, "ABS")) {
                    if (bound_args.size() != 1) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    if (!type::is_numeric(arg_types[0])) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    bound_func = func_type_t::ABS;
                    res_type   = arg_types[0];
                } else if (string_utils::iequals{}(name_view, "MOD")) {
                    if (bound_args.size() != 2) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    if (!type::is_numeric(arg_types[0]) || !type::is_numeric(arg_types[1])) {
                        return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                    }
                    auto common{type::common_type(arg_types[0], arg_types[1])};
                    if (!common) { return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}}; }
                    for (usize i{0}; i < 2; ++i) {
                        if (arg_types[i] != common) {
                            if (!type::can_coerce(arg_types[i], common)) {
                                return stdx::err{diagnostic{error::SQL_TYPE_MISMATCH, loc}};
                            }
                            bound_args[i] =
                                ast.template add_node<cast_expr_t>(loc, bound_args[i], *common);
                        }
                    }
                    bound_func = func_type_t::MOD;
                    res_type   = common;
                } else {
                    return stdx::err{diagnostic{error::SQL_COLUMN_NOT_FOUND, loc}};
                }

                return ast.template add_node<function_expr_t>(
                    loc, bound_func, std::move(bound_args), res_type);
            },
            [&](const auto&) -> stdx::result<node_id_t, diagnostic> {
                return stdx::err{diagnostic{error::IO_ERROR, loc}};
            });
    }

    [[nodiscard]] static auto get_expr_type(const ast_t& ast, node_id_t id) noexcept
        -> stdx::option<type::id_t> {
        return ast[id].visit(
            [](const literal_expr_t& lit) { return lit.type; },
            [](const column_ref_expr_t& col) { return col.type; },
            [](const binary_expr_t& bin) { return bin.type; },
            [](const cast_expr_t& cst) -> stdx::option<type::id_t> { return cst.target_type; },
            [](const aggregate_expr_t& agg) { return agg.return_type; },
            [](const unary_expr_t& unary) { return unary.type; },
            [](const function_expr_t& func) { return func.type; },
            [](const auto&) -> stdx::option<type::id_t> { return stdx::none; });
    }

    static auto collect_unaggregated_cols(const ast_t&                    ast,
                                          node_id_t                       id,
                                          std::vector<column_ref_expr_t>& cols) -> void {
        ast[id].visit(
            [&](const column_ref_expr_t& col) { cols.emplace_back(col); },
            [&](const binary_expr_t& bin) {
                collect_unaggregated_cols(ast, bin.lhs, cols);
                collect_unaggregated_cols(ast, bin.rhs, cols);
            },
            [&](const cast_expr_t& cst) { collect_unaggregated_cols(ast, cst.expr, cols); },
            [&](const unary_expr_t& unary) { collect_unaggregated_cols(ast, unary.expr, cols); },
            [&](const function_expr_t& func) {
                for (auto arg : func.args) { collect_unaggregated_cols(ast, arg, cols); }
            },
            [&](const aggregate_expr_t&) {},
            [&](const auto&) {});
    }

    [[nodiscard]] static auto contains_aggregate(const ast_t& ast, node_id_t id) -> bool {
        return ast[id].visit(
            [&](const aggregate_expr_t&) { return true; },
            [&](const binary_expr_t& bin) {
                return contains_aggregate(ast, bin.lhs) || contains_aggregate(ast, bin.rhs);
            },
            [&](const cast_expr_t& cst) { return contains_aggregate(ast, cst.expr); },
            [&](const unary_expr_t& unary) { return contains_aggregate(ast, unary.expr); },
            [&](const function_expr_t& func) {
                for (auto arg : func.args) {
                    if (contains_aggregate(ast, arg)) { return true; }
                }
                return false;
            },
            [&](const auto&) { return false; });
    }

  private:
    const catalog<PoolSize>& catalog_;
};

} // namespace cairn::sql::binder
