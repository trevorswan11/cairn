#include "sql/parser.hh"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tao/pegtl.hpp>
#include <tao/pegtl/position_with_source.hpp>
#include <tao/pegtl/text_position.hpp>

#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/profiler.hh>

#include "sql/ast.hh"
#include "sql/peg.hh"

namespace cairn::sql {

namespace peg = tao::pegtl;

namespace {

struct parser_state_t {
    ast::ast_t tree{};

    struct expr_scope_t {
        std::vector<ast::node_id_t> operands{};
        std::vector<ast::binary_op_t> operators{};
    };
    std::vector<expr_scope_t> expr_scopes{};

    std::vector<ast::select_item_t> select_list{};
    std::string                     table_name{""};
    std::string                     index_name{""};
    std::vector<std::string>        index_columns{};
    std::vector<ast::column_def_t>  column_defs{};
    ast::column_def_t               current_column_def{};
    type::id_t                      current_data_type{type::id_t::INVALID};
    bool                            column_nullable{true};

    auto active_scope() -> expr_scope_t& {
        if (expr_scopes.empty()) {
            expr_scopes.emplace_back();
        }
        return expr_scopes.back();
    }
};

auto clean_identifier(std::string_view sv) -> std::string {
    if (sv.size() >= 2 && (sv.front() == '`' || sv.front() == '"') && sv.back() == sv.front()) {
        return std::string{sv.substr(1, sv.size() - 2)};
    }
    return std::string{sv};
}

constexpr auto get_precedence(ast::binary_op_t op) noexcept -> int {
    switch (op) {
    case ast::binary_op_t::OR:  return 1;
    case ast::binary_op_t::AND: return 2;
    case ast::binary_op_t::EQUAL:
    case ast::binary_op_t::NOT_EQUAL:
    case ast::binary_op_t::LESS_THAN:
    case ast::binary_op_t::LESS_THAN_OR_EQUAL:
    case ast::binary_op_t::GREATER_THAN:
    case ast::binary_op_t::GREATER_THAN_OR_EQUAL: return 3;
    case ast::binary_op_t::ADD:
    case ast::binary_op_t::SUBTRACT: return 4;
    case ast::binary_op_t::MULTIPLY:
    case ast::binary_op_t::DIVIDE: return 5;
    }
    return 0;
}

auto parse_binary_op(std::string_view sv) noexcept -> ast::binary_op_t {
    if (sv == "+") return ast::binary_op_t::ADD;
    if (sv == "-") return ast::binary_op_t::SUBTRACT;
    if (sv == "*") return ast::binary_op_t::MULTIPLY;
    if (sv == "/") return ast::binary_op_t::DIVIDE;
    if (sv == "=") return ast::binary_op_t::EQUAL;
    if (sv == "!=" || sv == "<>") return ast::binary_op_t::NOT_EQUAL;
    if (sv == "<=") return ast::binary_op_t::LESS_THAN_OR_EQUAL;
    if (sv == ">=") return ast::binary_op_t::GREATER_THAN_OR_EQUAL;
    if (sv == "<") return ast::binary_op_t::LESS_THAN;
    if (sv == ">") return ast::binary_op_t::GREATER_THAN;

    if (sv.size() == 3 && (sv[0] == 'a' || sv[0] == 'A') && (sv[1] == 'n' || sv[1] == 'N') && (sv[2] == 'd' || sv[2] == 'D')) {
        return ast::binary_op_t::AND;
    }
    if (sv.size() == 2 && (sv[0] == 'o' || sv[0] == 'O') && (sv[1] == 'r' || sv[1] == 'R')) {
        return ast::binary_op_t::OR;
    }
    return ast::binary_op_t::ADD;
}

template <typename Rule>
struct action_t : peg::nothing<Rule> {};

template <>
struct action_t<grammar::null_kw> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        auto id{state.tree.add_node(ast::literal_expr_t{stdx::monostate{}})};
        state.active_scope().operands.push_back(id);
    }
};

template <>
struct action_t<grammar::true_kw> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        auto id{state.tree.add_node(ast::literal_expr_t{true})};
        state.active_scope().operands.push_back(id);
    }
};

template <>
struct action_t<grammar::false_kw> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        auto id{state.tree.add_node(ast::literal_expr_t{false})};
        state.active_scope().operands.push_back(id);
    }
};

template <>
struct action_t<grammar::string_literal> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        auto str{in.string()};
        if (str.size() >= 2 && str.front() == '\'' && str.back() == '\'') {
            str = str.substr(1, str.size() - 2);
        }
        std::string unescaped{""};
        unescaped.reserve(str.size());
        for (usize i{0}; i < str.size(); ++i) {
            if (str[i] == '\\' && i + 1 < str.size() && str[i+1] == '\'') {
                unescaped += '\'';
                ++i;
            } else {
                unescaped += str[i];
            }
        }
        auto id{state.tree.add_node(ast::literal_expr_t{std::move(unescaped)})};
        state.active_scope().operands.push_back(id);
    }
};

template <>
struct action_t<grammar::numeric_literal> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        auto str{in.string()};
        ast::node_id_t id{};
        if (str.find('.') != std::string::npos || str.find('e') != std::string::npos || str.find('E') != std::string::npos) {
            double val{std::stod(str)};
            id = state.tree.add_node(ast::literal_expr_t{val});
        } else {
            long long val{std::stoll(str)};
            id = state.tree.add_node(ast::literal_expr_t{static_cast<i64>(val)});
        }
        state.active_scope().operands.push_back(id);
    }
};

template <>
struct action_t<grammar::expr_identifier> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        auto id{state.tree.add_node(ast::identifier_expr_t{clean_identifier(in.string_view())})};
        state.active_scope().operands.push_back(id);
    }
};

template <>
struct action_t<grammar::open_paren> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        state.expr_scopes.emplace_back();
    }
};

template <>
struct action_t<grammar::close_paren> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        ASSERT(!state.expr_scopes.empty());
        auto scope{std::move(state.expr_scopes.back())};
        state.expr_scopes.pop_back();

        ASSERT(scope.operands.size() == 1);
        state.active_scope().operands.push_back(scope.operands.front());
    }
};

template <>
struct action_t<grammar::binary_op> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        auto op{parse_binary_op(in.string_view())};
        auto& scope{state.active_scope()};
        while (!scope.operators.empty() && get_precedence(scope.operators.back()) >= get_precedence(op)) {
            auto top_op{scope.operators.back()};
            scope.operators.pop_back();

            ASSERT(scope.operands.size() >= 2);
            auto rhs{scope.operands.back()};
            scope.operands.pop_back();
            auto lhs{scope.operands.back()};
            scope.operands.pop_back();

            auto id{state.tree.add_node(ast::binary_expr_t{top_op, lhs, rhs})};
            scope.operands.push_back(id);
        }
        scope.operators.push_back(op);
    }
};

template <>
struct action_t<grammar::expression> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        auto& scope{state.active_scope()};
        while (!scope.operators.empty()) {
            auto op{scope.operators.back()};
            scope.operators.pop_back();

            ASSERT(scope.operands.size() >= 2);
            auto rhs{scope.operands.back()};
            scope.operands.pop_back();
            auto lhs{scope.operands.back()};
            scope.operands.pop_back();

            auto id{state.tree.add_node(ast::binary_expr_t{op, lhs, rhs})};
            scope.operands.push_back(id);
        }
    }
};

template <typename Rule, type::id_t TypeID>
struct data_type_action {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        state.current_data_type = TypeID;
    }
};

template <> struct action_t<grammar::boolean_kw>  : data_type_action<grammar::boolean_kw, type::id_t::BOOLEAN> {};
template <> struct action_t<grammar::tinyint_kw>  : data_type_action<grammar::tinyint_kw, type::id_t::TINYINT> {};
template <> struct action_t<grammar::smallint_kw> : data_type_action<grammar::smallint_kw, type::id_t::SMALLINT> {};
template <> struct action_t<grammar::integer_kw>  : data_type_action<grammar::integer_kw, type::id_t::INTEGER> {};
template <> struct action_t<grammar::int_kw>      : data_type_action<grammar::int_kw, type::id_t::INTEGER> {};
template <> struct action_t<grammar::bigint_kw>   : data_type_action<grammar::bigint_kw, type::id_t::BIGINT> {};
template <> struct action_t<grammar::float_kw>    : data_type_action<grammar::float_kw, type::id_t::FLOAT> {};
template <> struct action_t<grammar::double_kw>   : data_type_action<grammar::double_kw, type::id_t::DOUBLE> {};
template <> struct action_t<grammar::varchar_kw>  : data_type_action<grammar::varchar_kw, type::id_t::VARCHAR> {};
template <> struct action_t<grammar::datetime_kw> : data_type_action<grammar::datetime_kw, type::id_t::DATETIME> {};

template <>
struct action_t<grammar::not_kw> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        state.column_nullable = false;
    }
};

template <>
struct action_t<grammar::column_name> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.current_column_def.name = clean_identifier(in.string_view());
        state.column_nullable = true;
    }
};

template <>
struct action_t<grammar::column_def> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        state.current_column_def.type = state.current_data_type;
        state.current_column_def.nullable = state.column_nullable;
        state.column_defs.push_back(std::move(state.current_column_def));
    }
};

template <>
struct action_t<grammar::select_table> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <>
struct action_t<grammar::create_table_name> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <>
struct action_t<grammar::drop_table_name> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <>
struct action_t<grammar::alter_table_name> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <>
struct action_t<grammar::create_index_name> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.index_name = clean_identifier(in.string_view());
    }
};

template <>
struct action_t<grammar::create_index_table_name> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <>
struct action_t<grammar::drop_index_name> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.index_name = clean_identifier(in.string_view());
    }
};

template <>
struct action_t<grammar::drop_index_table_name> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <>
struct action_t<grammar::index_column> {
    template <typename ActionInput>
    static void apply(const ActionInput& in, parser_state_t& state) {
        state.index_columns.push_back(clean_identifier(in.string_view()));
    }
};

template <>
struct action_t<grammar::select_all> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        state.select_list.push_back({.is_star = true, .expr = ast::node_id_t::make_invalid()});
    }
};

template <>
struct action_t<grammar::select_item> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        if (!state.active_scope().operands.empty()) {
            auto expr{state.active_scope().operands.back()};
            state.active_scope().operands.pop_back();
            state.select_list.push_back({.is_star = false, .expr = expr});
        }
    }
};

template <>
struct action_t<grammar::select_stmt> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        auto where_clause{ast::node_id_t::make_invalid()};
        if (!state.active_scope().operands.empty()) {
            where_clause = state.active_scope().operands.back();
            state.active_scope().operands.pop_back();
        }
        auto id{state.tree.add_node(ast::select_stmt_t{
            std::move(state.select_list),
            std::move(state.table_name),
            where_clause
        })};
        state.tree.add_root(id);
    }
};

template <>
struct action_t<grammar::create_table_stmt> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        auto id{state.tree.add_node(ast::create_table_stmt_t{
            std::move(state.table_name),
            std::move(state.column_defs)
        })};
        state.tree.add_root(id);
    }
};

template <>
struct action_t<grammar::drop_table_stmt> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        auto id{state.tree.add_node(ast::drop_table_stmt_t{std::move(state.table_name)})};
        state.tree.add_root(id);
    }
};

template <>
struct action_t<grammar::alter_table_stmt> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        ASSERT(!state.column_defs.empty());
        auto col_def{std::move(state.column_defs.back())};
        state.column_defs.pop_back();
        auto id{state.tree.add_node(ast::alter_table_stmt_t{
            std::move(state.table_name),
            std::move(col_def)
        })};
        state.tree.add_root(id);
    }
};

template <>
struct action_t<grammar::create_index_stmt> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        auto id{state.tree.add_node(ast::create_index_stmt_t{
            std::move(state.index_name),
            std::move(state.table_name),
            std::move(state.index_columns)
        })};
        state.tree.add_root(id);
    }
};

template <>
struct action_t<grammar::drop_index_stmt> {
    template <typename ActionInput>
    static void apply(const ActionInput&, parser_state_t& state) {
        auto id{state.tree.add_node(ast::drop_index_stmt_t{
            std::move(state.index_name),
            std::move(state.table_name)
        })};
        state.tree.add_root(id);
    }
};

} // namespace

auto parse(const file& source_file) noexcept -> parse_result_t {
    PROFILE_FUNCTION();

    std::string_view query_view{source_file};
    peg::text_view_input in{"SQL Query", query_view};
    parser_state_t state{};
    parse_diags_t diags{};

    try {
        if (peg::parse<grammar::sql_grammar, action_t>(in, state)) {
            return {std::move(state.tree), std::move(diags)};
        }
        diags.emplace_back(0, 0);
        return {stdx::err{error::SQL_SYNTAX_ERROR}, std::move(diags)};
    }
    catch (const peg::parse_error<peg::position_with_source<std::string, peg::text_position>>& e) {
        const auto& pos{e.position_object()};
        diags.emplace_back(pos.line - 1, pos.column - 1);
        return {stdx::err{error::SQL_SYNTAX_ERROR}, std::move(diags)};
    }
    catch (...) {
        diags.emplace_back(0, 0);
        return {stdx::err{error::SQL_SYNTAX_ERROR}, std::move(diags)};
    }
}

} // namespace cairn::sql
