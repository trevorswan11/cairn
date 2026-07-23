#include "sql/parser/parser.hh"

#include <array>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <stdx/assert.hh>
#include <stdx/fixed/enum_map.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/fixed/string.hh>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>
#include <tao/pegtl/nothing.hpp>
#include <tao/pegtl/parse.hpp>
#include <tao/pegtl/parse_error.hpp>
#include <tao/pegtl/position_with_source.hpp>
#include <tao/pegtl/text_position.hpp>
#include <tao/pegtl/text_view_input.hpp>

#include "parser/grammar.hh"
#include "sql/detail/node.hh"
#include "sql/file.hh"
#include "sql/type.hh"
#include "support/diagnostic/location.hh"
#include "support/string_utils.hh"

namespace cairn {

template <>
struct source_info<tao::pegtl::position_with_source<std::string, tao::pegtl::text_position>> {
    using pos_t = tao::pegtl::position_with_source<std::string, tao::pegtl::text_position>;
    static auto get(const pos_t& loc) -> location { return location{loc.line - 1, loc.column - 1}; }
};

namespace sql::parser {

namespace peg = tao::pegtl;

namespace {

struct expr_scope_t {
    std::vector<node_id_t>   operands;
    std::vector<binary_op_t> operators;
};

struct parser_state_t {
    ast_t tree;

    std::vector<expr_scope_t>         expr_scopes;
    std::vector<select_item_t>        select_list;
    stdx::fixed::string               table_name;
    stdx::option<stdx::fixed::string> table_alias;
    stdx::option<node_id_t>           where_clause;
    std::vector<node_id_t>            group_by_list;
    stdx::option<node_id_t>           having_clause;
    stdx::option<agg_func_t>          current_agg_func;
    bool                              is_distinct_agg{false};
    stdx::fixed::string               index_name;
    std::vector<stdx::fixed::string>  index_columns;
    std::vector<column_def_t>         column_defs;
    column_def_t                      current_column_def;
    stdx::option<type::id_t>          current_data_type;
    bool                              column_nullable{true};

    [[nodiscard]] auto active_scope() -> expr_scope_t& {
        return expr_scopes.empty() ? expr_scopes.emplace_back() : expr_scopes.back();
    }
};

[[nodiscard]] auto clean_identifier(std::string_view sv) -> stdx::fixed::string {
    PROFILE_FUNCTION();
    if (sv.size() >= 2 && (sv.starts_with('`') || sv.starts_with('"')) && sv.back() == sv.front()) {
        return stdx::string::substr(sv, 1, sv.size() - 2);
    }
    return sv;
}

constexpr auto precedence_map{[] {
    stdx::fixed::enum_map<binary_op_t, i32> map{0};
    map[binary_op_t::OR]   = 1;
    map[binary_op_t::AND]  = 2;
    map[binary_op_t::EQ]   = 3;
    map[binary_op_t::NEQ]  = 3;
    map[binary_op_t::LT]   = 3;
    map[binary_op_t::LTEQ] = 3;
    map[binary_op_t::GT]   = 3;
    map[binary_op_t::GTEQ] = 3;
    map[binary_op_t::ADD]  = 4;
    map[binary_op_t::SUB]  = 4;
    map[binary_op_t::MUL]  = 5;
    map[binary_op_t::DIV]  = 5;
    map[binary_op_t::MOD]  = 5;
    return map;
}()};

constexpr auto binary_ops{[] {
    constexpr std::array operators{std::pair{"+", binary_op_t::ADD},
                                   std::pair{"-", binary_op_t::SUB},
                                   std::pair{"*", binary_op_t::MUL},
                                   std::pair{"/", binary_op_t::DIV},
                                   std::pair{"%", binary_op_t::MOD},
                                   std::pair{"=", binary_op_t::EQ},
                                   std::pair{"!=", binary_op_t::NEQ},
                                   std::pair{"<>", binary_op_t::NEQ},
                                   std::pair{"<=", binary_op_t::LTEQ},
                                   std::pair{">=", binary_op_t::GTEQ},
                                   std::pair{"<", binary_op_t::LT},
                                   std::pair{">", binary_op_t::GT},
                                   std::pair{"and", binary_op_t::AND},
                                   std::pair{"or", binary_op_t::OR}};

    stdx::fixed::hash_map<std::string_view,
                          binary_op_t,
                          operators.size(),
                          string_utils::ihash,
                          string_utils::iequals>
        map;
    for (const auto& op : operators) { map.emplace(op.first, op.second); }
    return map;
}()};

template <typename Rule> struct action_t : peg::nothing<Rule> {};

template <> struct action_t<grammar::null_kw> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<literal_expr_t>(in.current_position())};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::true_kw> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<literal_expr_t>(in.current_position(), true)};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::false_kw> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<literal_expr_t>(in.current_position(), false)};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::string_literal> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto str{in.string_view()};
        if (str.size() >= 2 && str.starts_with('\'') && str.front() == str.back()) {
            str = stdx::string::substr(str, 1, str.size() - 2);
        }

        stdx::fixed::string unescaped{str.size()};
        for (usize i{0}; i < str.size(); ++i) {
            if (str[i] == '\\' && i + 1 < str.size() && str[i + 1] == '\'') {
                unescaped[i] = '\'';
                ++i;
            } else {
                unescaped[i] = str[i];
            }
        }
        auto id{state.tree.add_node<literal_expr_t>(in.current_position(), std::move(unescaped))};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::numeric_literal> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto      str{in.string_view()};
        node_id_t id;
        if (str.contains('.') || str.contains('e') || str.contains('E')) {
            double value;
            auto   result{std::from_chars(str.begin(), str.end(), value)};
            ASSERT(result.ec == std::errc{} && result.ptr == str.end());
            id = state.tree.add_node<literal_expr_t>(in.current_position(), value);
        } else {
            i64  value;
            auto result{std::from_chars(str.begin(), str.end(), value)};
            ASSERT(result.ec == std::errc{} && result.ptr == str.end());
            id = state.tree.add_node<literal_expr_t>(in.current_position(), value);
        }
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::expr_identifier> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<identifier_expr_t>(in.current_position(),
                                                       clean_identifier(in.string_view()))};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::func_open_paren> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.expr_scopes.emplace_back();
    }
};

template <> struct action_t<grammar::function_call> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        ASSERT(!state.expr_scopes.empty());
        auto scope{std::move(state.expr_scopes.back())};
        state.expr_scopes.pop_back();

        auto matched_str{in.string_view()};
        auto first_paren{matched_str.find('(')};
        ASSERT(first_paren != std::string_view::npos);
        auto name{clean_identifier(stdx::string::substr(matched_str, 0, first_paren))};

        auto id{state.tree.add_node<function_expr_t>(
            in.current_position(), name, std::move(scope.operands))};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::unary_minus_expr> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto& operands{state.active_scope().operands};
        ASSERT(!operands.empty());
        auto child{operands.back()};
        operands.pop_back();

        auto id{state.tree.add_node<unary_expr_t>(in.current_position(), unary_op_t::MINUS, child)};
        operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::unary_not_expr> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto& operands{state.active_scope().operands};
        ASSERT(!operands.empty());
        auto child{operands.back()};
        operands.pop_back();

        auto id{state.tree.add_node<unary_expr_t>(in.current_position(), unary_op_t::NOT, child)};
        operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::is_null_op> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto& operands{state.active_scope().operands};
        ASSERT(!operands.empty());
        auto child{operands.back()};
        operands.pop_back();

        auto id{
            state.tree.add_node<unary_expr_t>(in.current_position(), unary_op_t::IS_NULL, child)};
        operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::is_not_null_op> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto& operands{state.active_scope().operands};
        ASSERT(!operands.empty());
        auto child{operands.back()};
        operands.pop_back();

        auto id{state.tree.add_node<unary_expr_t>(
            in.current_position(), unary_op_t::IS_NOT_NULL, child)};
        operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::open_paren> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.expr_scopes.emplace_back();
    }
};

template <> struct action_t<grammar::close_paren> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        ASSERT(!state.expr_scopes.empty());
        auto scope{std::move(state.expr_scopes.back())};
        state.expr_scopes.pop_back();

        ASSERT(scope.operands.size() == 1);
        state.active_scope().operands.emplace_back(scope.operands.front());
    }
};

template <> struct action_t<grammar::binary_op> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto  op{binary_ops.get_opt(in.string_view()).materialize().value_or(binary_op_t::ADD)};
        auto& scope{state.active_scope()};
        while (!scope.operators.empty()) {
            if (precedence_map[scope.operators.back()] < precedence_map[op]) { break; }
            auto top_op{scope.operators.back()};
            scope.operators.pop_back();

            ASSERT(scope.operands.size() >= 2);
            auto rhs{scope.operands.back()};
            scope.operands.pop_back();
            auto lhs{scope.operands.back()};
            scope.operands.pop_back();

            auto id{state.tree.add_node<binary_expr_t>(in.current_position(), top_op, lhs, rhs)};
            scope.operands.emplace_back(id);
        }
        scope.operators.emplace_back(op);
    }
};

template <> struct action_t<grammar::expression> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto& scope{state.active_scope()};
        while (!scope.operators.empty()) {
            auto op{scope.operators.back()};
            scope.operators.pop_back();

            ASSERT(scope.operands.size() >= 2);
            auto rhs{scope.operands.back()};
            scope.operands.pop_back();
            auto lhs{scope.operands.back()};
            scope.operands.pop_back();

            auto id{state.tree.add_node<binary_expr_t>(in.current_position(), op, lhs, rhs)};
            scope.operands.emplace_back(id);
        }
    }
};

template <typename Rule, type::id_t TypeID> struct data_type_action {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.current_data_type = TypeID;
    }
};

// clang-format off
template <> struct action_t<grammar::boolean_kw> : data_type_action<grammar::boolean_kw, type::id_t::BOOLEAN> {};
template <> struct action_t<grammar::tinyint_kw> : data_type_action<grammar::tinyint_kw, type::id_t::TINYINT> {};
template <> struct action_t<grammar::smallint_kw> : data_type_action<grammar::smallint_kw, type::id_t::SMALLINT> {};
template <> struct action_t<grammar::integer_kw> : data_type_action<grammar::integer_kw, type::id_t::INTEGER> {};
template <> struct action_t<grammar::int_kw> : data_type_action<grammar::int_kw, type::id_t::INTEGER> {};
template <> struct action_t<grammar::bigint_kw> : data_type_action<grammar::bigint_kw, type::id_t::BIGINT> {};
template <> struct action_t<grammar::float_kw> : data_type_action<grammar::float_kw, type::id_t::FLOAT> {};
template <> struct action_t<grammar::double_kw> : data_type_action<grammar::double_kw, type::id_t::DOUBLE> {};
template <> struct action_t<grammar::varchar_kw> : data_type_action<grammar::varchar_kw, type::id_t::VARCHAR> {};
template <> struct action_t<grammar::datetime_kw> : data_type_action<grammar::datetime_kw, type::id_t::DATETIME> {};
// clang-format on

template <> struct action_t<grammar::not_kw> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.column_nullable = false;
    }
};

template <> struct action_t<grammar::column_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.current_column_def.name = clean_identifier(in.string_view());
        state.column_nullable         = true;
    }
};

template <> struct action_t<grammar::column_def> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.current_column_def.type     = *state.current_data_type;
        state.current_column_def.nullable = state.column_nullable;
        state.column_defs.emplace_back(std::move(state.current_column_def));
    }
};

template <> struct action_t<grammar::select_table> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::table_alias> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.table_alias = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::create_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::drop_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::alter_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::create_index_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.index_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::create_index_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::drop_index_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.index_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::drop_index_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::index_column> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.index_columns.emplace_back(clean_identifier(in.string_view()));
    }
};

template <> struct action_t<grammar::select_all> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.select_list.emplace_back(select_item_t{stdx::none});
    }
};

template <> struct action_t<grammar::select_item> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        if (!state.active_scope().operands.empty()) {
            auto expr{state.active_scope().operands.back()};
            state.active_scope().operands.pop_back();
            state.select_list.emplace_back(select_item_t{expr});
        }
    }
};

template <> struct action_t<grammar::count_star_expr> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<aggregate_expr_t>(
            in.current_position(), agg_func_t::COUNT, stdx::none, false)};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::agg_func_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto sv{in.string_view()};
        if (string_utils::iequals{}(sv, "COUNT")) {
            state.current_agg_func = agg_func_t::COUNT;
        } else if (string_utils::iequals{}(sv, "SUM")) {
            state.current_agg_func = agg_func_t::SUM;
        } else if (string_utils::iequals{}(sv, "AVG")) {
            state.current_agg_func = agg_func_t::AVG;
        } else if (string_utils::iequals{}(sv, "MIN")) {
            state.current_agg_func = agg_func_t::MIN;
        } else if (string_utils::iequals{}(sv, "MAX")) {
            state.current_agg_func = agg_func_t::MAX;
        }
    }
};

template <> struct action_t<grammar::agg_open_paren> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.expr_scopes.emplace_back();
    }
};

template <> struct action_t<grammar::agg_close_paren> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        ASSERT(!state.expr_scopes.empty());
        auto scope{std::move(state.expr_scopes.back())};
        state.expr_scopes.pop_back();

        if (!scope.operands.empty()) {
            ASSERT(scope.operands.size() == 1);
            state.active_scope().operands.emplace_back(scope.operands.front());
        }
    }
};

template <> struct action_t<grammar::distinct_kw> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.is_distinct_agg = true;
    }
};

template <> struct action_t<grammar::aggregate_expr> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        if (!state.current_agg_func) { return; }
        auto func{*state.current_agg_func};
        state.current_agg_func = stdx::none;
        bool is_distinct{state.is_distinct_agg};
        state.is_distinct_agg = false;

        stdx::option<node_id_t> arg;
        if (!state.active_scope().operands.empty()) {
            arg = state.active_scope().operands.back();
            state.active_scope().operands.pop_back();
        }

        auto id{
            state.tree.add_node<aggregate_expr_t>(in.current_position(), func, arg, is_distinct)};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::where_clause_rule> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        if (!state.active_scope().operands.empty()) {
            state.where_clause = state.active_scope().operands.back();
            state.active_scope().operands.pop_back();
        }
    }
};

template <> struct action_t<grammar::group_by_expr> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        if (!state.active_scope().operands.empty()) {
            auto expr{state.active_scope().operands.back()};
            state.active_scope().operands.pop_back();
            state.group_by_list.emplace_back(expr);
        }
    }
};

template <> struct action_t<grammar::having_clause_rule> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        if (!state.active_scope().operands.empty()) {
            state.having_clause = state.active_scope().operands.back();
            state.active_scope().operands.pop_back();
        }
    }
};

template <> struct action_t<grammar::select_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<select_stmt_t>(in.current_position(),
                                                   std::move(state.select_list),
                                                   std::move(state.table_name),
                                                   std::move(state.table_alias),
                                                   state.where_clause,
                                                   std::move(state.group_by_list),
                                                   state.having_clause)};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::create_table_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<create_table_stmt_t>(
            in.current_position(), std::move(state.table_name), std::move(state.column_defs))};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::drop_table_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<drop_table_stmt_t>(in.current_position(),
                                                       std::move(state.table_name))};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::alter_column_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        state.current_column_def.name = clean_identifier(in.string_view());
        state.current_column_def.type.reset();
        state.current_column_def.nullable = true;
        state.column_defs.emplace_back(std::move(state.current_column_def));
    }
};

template <> struct action_t<grammar::alter_table_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        ASSERT(!state.column_defs.empty());
        auto col_def{std::move(state.column_defs.back())};
        state.column_defs.pop_back();
        auto id{state.tree.add_node<alter_table_stmt_t>(
            in.current_position(), std::move(state.table_name), std::move(col_def))};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::create_index_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<create_index_stmt_t>(in.current_position(),
                                                         std::move(state.index_name),
                                                         std::move(state.table_name),
                                                         std::move(state.index_columns))};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::drop_index_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        PROFILE_FUNCTION();
        auto id{state.tree.add_node<drop_index_stmt_t>(
            in.current_position(), std::move(state.index_name), std::move(state.table_name))};
        state.tree.add_root(id);
    }
};

} // namespace

auto parse(const file& source_file) noexcept -> stdx::result<ast_t, location> {
    PROFILE_FUNCTION();

    std::string_view     query_view{source_file};
    peg::text_view_input in{"SQL Query", query_view};
    parser_state_t       state;

    try {
        if (peg::parse<grammar::sql_grammar, action_t>(in, state)) { return std::move(state.tree); }
        return stdx::err{location{0, 0}};
    } catch (
        const peg::parse_error<peg::position_with_source<std::string, peg::text_position>>& e) {
        const auto& pos{e.position_object()};
        return stdx::err{location{pos.line - 1, pos.column - 1}};
    } catch (...) { return stdx::err{location{0, 0}}; }
}

} // namespace sql::parser

} // namespace cairn
