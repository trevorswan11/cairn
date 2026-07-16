#include "sql/parser.hh"

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
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>
#include <tao/pegtl.hpp>
#include <tao/pegtl/nothing.hpp>
#include <tao/pegtl/parse.hpp>
#include <tao/pegtl/parse_error.hpp>
#include <tao/pegtl/position_with_source.hpp>
#include <tao/pegtl/text_position.hpp>
#include <tao/pegtl/text_view_input.hpp>

#include "sql/ast.hh"
#include "sql/file.hh"
#include "sql/peg.hh"
#include "sql/type.hh"
#include "stdx/option.hh"
#include "support/diagnostic/location.hh"
#include "support/string_utils.hh"

namespace cairn::sql {

namespace peg = tao::pegtl;

namespace {

struct parser_state_t {
    ast::ast_t tree;

    struct expr_scope_t {
        std::vector<ast::node_id_t>   operands;
        std::vector<ast::binary_op_t> operators;
    };
    std::vector<expr_scope_t> expr_scopes;

    std::vector<ast::select_item_t>  select_list;
    stdx::fixed::string              table_name;
    stdx::fixed::string              index_name;
    std::vector<stdx::fixed::string> index_columns;
    std::vector<ast::column_def_t>   column_defs;
    ast::column_def_t                current_column_def;
    stdx::option<type::id_t>         current_data_type;
    bool                             column_nullable{true};

    [[nodiscard]] auto active_scope() -> expr_scope_t& {
        return expr_scopes.empty() ? expr_scopes.emplace_back() : expr_scopes.back();
    }
};

[[nodiscard]] auto clean_identifier(std::string_view sv) -> stdx::fixed::string {
    if (sv.size() >= 2 && (sv.starts_with('`') || sv.starts_with('"')) && sv.back() == sv.front()) {
        return stdx::fixed::string{stdx::string::substr(sv, 1, sv.size() - 2)};
    }
    return stdx::fixed::string{sv};
}

constexpr auto precedence_map{[] {
    stdx::fixed::enum_map<ast::binary_op_t, i32> map{0};
    map[ast::binary_op_t::OR]                    = 1;
    map[ast::binary_op_t::AND]                   = 2;
    map[ast::binary_op_t::EQUAL]                 = 3;
    map[ast::binary_op_t::NOT_EQUAL]             = 3;
    map[ast::binary_op_t::LESS_THAN]             = 3;
    map[ast::binary_op_t::LESS_THAN_OR_EQUAL]    = 3;
    map[ast::binary_op_t::GREATER_THAN]          = 3;
    map[ast::binary_op_t::GREATER_THAN_OR_EQUAL] = 3;
    map[ast::binary_op_t::ADD]                   = 4;
    map[ast::binary_op_t::SUBTRACT]              = 4;
    map[ast::binary_op_t::MULTIPLY]              = 5;
    map[ast::binary_op_t::DIVIDE]                = 5;
    return map;
}()};

constexpr auto binary_ops{[] {
    constexpr std::array operators{std::pair{"+", ast::binary_op_t::ADD},
                                   std::pair{"-", ast::binary_op_t::SUBTRACT},
                                   std::pair{"*", ast::binary_op_t::MULTIPLY},
                                   std::pair{"/", ast::binary_op_t::DIVIDE},
                                   std::pair{"=", ast::binary_op_t::EQUAL},
                                   std::pair{"!=", ast::binary_op_t::NOT_EQUAL},
                                   std::pair{"<>", ast::binary_op_t::NOT_EQUAL},
                                   std::pair{"<=", ast::binary_op_t::LESS_THAN_OR_EQUAL},
                                   std::pair{">=", ast::binary_op_t::GREATER_THAN_OR_EQUAL},
                                   std::pair{"<", ast::binary_op_t::LESS_THAN},
                                   std::pair{">", ast::binary_op_t::GREATER_THAN},
                                   std::pair{"and", ast::binary_op_t::AND},
                                   std::pair{"or", ast::binary_op_t::OR}};

    stdx::fixed::hash_map<std::string_view,
                          ast::binary_op_t,
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
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        auto id{state.tree.add_node<ast::literal_expr_t>()};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::true_kw> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        auto id{state.tree.add_node<ast::literal_expr_t>(true)};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::false_kw> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        auto id{state.tree.add_node<ast::literal_expr_t>(false)};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::string_literal> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
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
        auto id{state.tree.add_node<ast::literal_expr_t>(std::move(unescaped))};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::numeric_literal> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        auto           str{in.string_view()};
        ast::node_id_t id;
        if (str.contains('.') || str.contains('e') || str.contains('E')) {
            double value;
            auto   result{std::from_chars(str.begin(), str.end(), value)};
            VERIFY(result.ec == std::errc{} && result.ptr == str.end());
            id = state.tree.add_node<ast::literal_expr_t>(value);
        } else {
            i64  value;
            auto result{std::from_chars(str.begin(), str.end(), value)};
            VERIFY(result.ec == std::errc{} && result.ptr == str.end());
            id = state.tree.add_node<ast::literal_expr_t>(value);
        }
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::expr_identifier> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        auto id{state.tree.add_node<ast::identifier_expr_t>(clean_identifier(in.string_view()))};
        state.active_scope().operands.emplace_back(id);
    }
};

template <> struct action_t<grammar::open_paren> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        state.expr_scopes.emplace_back();
    }
};

template <> struct action_t<grammar::close_paren> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
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
        auto op{binary_ops.get_opt(in.string_view()).materialize().value_or(ast::binary_op_t::ADD)};
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

            auto id{state.tree.add_node<ast::binary_expr_t>(top_op, lhs, rhs)};
            scope.operands.emplace_back(id);
        }
        scope.operators.emplace_back(op);
    }
};

template <> struct action_t<grammar::expression> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        auto& scope{state.active_scope()};
        while (!scope.operators.empty()) {
            auto op{scope.operators.back()};
            scope.operators.pop_back();

            ASSERT(scope.operands.size() >= 2);
            auto rhs{scope.operands.back()};
            scope.operands.pop_back();
            auto lhs{scope.operands.back()};
            scope.operands.pop_back();

            auto id{state.tree.add_node<ast::binary_expr_t>(op, lhs, rhs)};
            scope.operands.emplace_back(id);
        }
    }
};

template <typename Rule, type::id_t TypeID> struct data_type_action {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        state.current_data_type = TypeID;
    }
};

template <>
struct action_t<grammar::boolean_kw> : data_type_action<grammar::boolean_kw, type::id_t::BOOLEAN> {
};
template <>
struct action_t<grammar::tinyint_kw> : data_type_action<grammar::tinyint_kw, type::id_t::TINYINT> {
};
template <>
struct action_t<grammar::smallint_kw>
    : data_type_action<grammar::smallint_kw, type::id_t::SMALLINT> {};
template <>
struct action_t<grammar::integer_kw> : data_type_action<grammar::integer_kw, type::id_t::INTEGER> {
};
template <>
struct action_t<grammar::int_kw> : data_type_action<grammar::int_kw, type::id_t::INTEGER> {};
template <>
struct action_t<grammar::bigint_kw> : data_type_action<grammar::bigint_kw, type::id_t::BIGINT> {};
template <>
struct action_t<grammar::float_kw> : data_type_action<grammar::float_kw, type::id_t::FLOAT> {};
template <>
struct action_t<grammar::double_kw> : data_type_action<grammar::double_kw, type::id_t::DOUBLE> {};
template <>
struct action_t<grammar::varchar_kw> : data_type_action<grammar::varchar_kw, type::id_t::VARCHAR> {
};
template <>
struct action_t<grammar::datetime_kw>
    : data_type_action<grammar::datetime_kw, type::id_t::DATETIME> {};

template <> struct action_t<grammar::not_kw> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        state.column_nullable = false;
    }
};

template <> struct action_t<grammar::column_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.current_column_def.name = clean_identifier(in.string_view());
        state.column_nullable         = true;
    }
};

template <> struct action_t<grammar::column_def> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        state.current_column_def.type     = *state.current_data_type;
        state.current_column_def.nullable = state.column_nullable;
        state.column_defs.emplace_back(std::move(state.current_column_def));
    }
};

template <> struct action_t<grammar::select_table> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::create_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::drop_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::alter_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::create_index_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.index_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::create_index_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::drop_index_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.index_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::drop_index_table_name> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.table_name = clean_identifier(in.string_view());
    }
};

template <> struct action_t<grammar::index_column> {
    template <typename ActionInput>
    static auto apply(const ActionInput& in, parser_state_t& state) -> void {
        state.index_columns.emplace_back(clean_identifier(in.string_view()));
    }
};

template <> struct action_t<grammar::select_all> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        state.select_list.emplace_back(ast::select_item_t{stdx::none});
    }
};

template <> struct action_t<grammar::select_item> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        if (!state.active_scope().operands.empty()) {
            auto expr{state.active_scope().operands.back()};
            state.active_scope().operands.pop_back();
            state.select_list.emplace_back(ast::select_item_t{expr});
        }
    }
};

template <> struct action_t<grammar::select_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        stdx::option<ast::node_id_t> where_clause;
        if (!state.active_scope().operands.empty()) {
            where_clause = state.active_scope().operands.back();
            state.active_scope().operands.pop_back();
        }
        auto id{state.tree.add_node<ast::select_stmt_t>(
            std::move(state.select_list), std::move(state.table_name), where_clause)};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::create_table_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        auto id{state.tree.add_node<ast::create_table_stmt_t>(std::move(state.table_name),
                                                              std::move(state.column_defs))};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::drop_table_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        auto id{state.tree.add_node<ast::drop_table_stmt_t>(std::move(state.table_name))};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::alter_table_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        ASSERT(!state.column_defs.empty());
        auto col_def{std::move(state.column_defs.back())};
        state.column_defs.pop_back();
        auto id{state.tree.add_node<ast::alter_table_stmt_t>(std::move(state.table_name),
                                                             std::move(col_def))};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::create_index_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        auto id{state.tree.add_node<ast::create_index_stmt_t>(std::move(state.index_name),
                                                              std::move(state.table_name),
                                                              std::move(state.index_columns))};
        state.tree.add_root(id);
    }
};

template <> struct action_t<grammar::drop_index_stmt> {
    template <typename ActionInput>
    static auto apply(const ActionInput&, parser_state_t& state) -> void {
        auto id{state.tree.add_node<ast::drop_index_stmt_t>(std::move(state.index_name),
                                                            std::move(state.table_name))};
        state.tree.add_root(id);
    }
};

} // namespace

auto parse(const file& source_file) noexcept -> parse_result_t {
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

} // namespace cairn::sql
