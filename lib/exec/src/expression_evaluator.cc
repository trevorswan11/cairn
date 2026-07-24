#include "exec/expression_evaluator.hh"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <concepts>
#include <string>
#include <string_view>
#include <system_error>

#include <stdx/assert.hh>
#include <stdx/fixed/string.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "sql/binder/nodes.hh"
#include "sql/parser/parser.hh"
#include "sql/tuple.hh"
#include "sql/type.hh"
#include "sql/value.hh"
#include "support/diagnostic/error.hh"
#include "support/string_utils.hh"

namespace cairn::exec {

namespace {

// Source - https://codereview.stackexchange.com/a/283360
template <std::floating_point ValueType>
[[nodiscard]]
constexpr auto float_approx_eq(ValueType l, ValueType r) noexcept -> bool {
    return r == std::nextafter(l, r);
}

} // namespace

template <typename TargetType>
[[nodiscard]] auto cast_to_numeric(const sql::value_t& val) -> result<sql::value_t> {
    return val.get_value().visit(
        [](bool v) -> result<sql::value_t> {
            return sql::value_t{static_cast<TargetType>(v ? 1 : 0)};
        },
        [](std::string_view v) -> result<sql::value_t> {
            if constexpr (std::integral<TargetType>) {
                i64        parsed;
                const auto res{std::from_chars(v.data(), v.data() + v.size(), parsed)};
                if (res.ec != std::errc{} || res.ptr != v.data() + v.size()) {
                    return stdx::err{error::SQL_TYPE_MISMATCH};
                }
                return sql::value_t{static_cast<TargetType>(parsed)};
            } else {
                f64        parsed;
                const auto res{std::from_chars(v.begin(), v.end(), parsed)};
                if (res.ec != std::errc{} || res.ptr != v.end()) {
                    return stdx::err{error::SQL_TYPE_MISMATCH};
                }
                return sql::value_t{static_cast<TargetType>(parsed)};
            }
        },
        [](auto v) -> result<sql::value_t> {
            if constexpr (requires { static_cast<TargetType>(v); }) {
                return sql::value_t{static_cast<TargetType>(v)};
            } else {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            }
        });
}

auto expression_evaluator_t::cast_value(const sql::value_t& val, sql::type::id_t target_type)
    -> result<sql::value_t> {
    if (val.is_null()) {
        return sql::value_t::make_null(target_type);
    } else if (val.type() == target_type) {
        return val;
    }

    const auto& value{val.get_value()};
    switch (target_type) {
    case sql::type::id_t::BOOLEAN:
        return value.visit(
            [](bool v) -> result<sql::value_t> { return sql::value_t{v}; },
            [](i8 v) -> result<sql::value_t> { return sql::value_t{v != 0}; },
            [](i16 v) -> result<sql::value_t> { return sql::value_t{v != 0}; },
            [](i32 v) -> result<sql::value_t> { return sql::value_t{v != 0}; },
            [](i64 v) -> result<sql::value_t> { return sql::value_t{v != 0}; },
            [](f32 v) -> result<sql::value_t> { return sql::value_t{!float_approx_eq(v, 0.0f)}; },
            [](f64 v) -> result<sql::value_t> { return sql::value_t{!float_approx_eq(v, 0.0)}; },
            [](std::string_view v) -> result<sql::value_t> {
                if (v == "1" || string_utils::iequals{}(v, "true")) {
                    return sql::value_t{true};
                } else if (v == "0" || string_utils::iequals{}(v, "false")) {
                    return sql::value_t{false};
                }
                return stdx::err{error::SQL_TYPE_MISMATCH};
            },
            [](const auto&) -> result<sql::value_t> {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            });
    case sql::type::id_t::TINYINT:  return cast_to_numeric<i8>(val);
    case sql::type::id_t::SMALLINT: return cast_to_numeric<i16>(val);
    case sql::type::id_t::INTEGER:  return cast_to_numeric<i32>(val);
    case sql::type::id_t::BIGINT:   return cast_to_numeric<i64>(val);
    case sql::type::id_t::FLOAT:    return cast_to_numeric<f32>(val);
    case sql::type::id_t::DOUBLE:   return cast_to_numeric<f64>(val);
    case sql::type::id_t::VARCHAR:
        return value.visit(
            [](stdx::monostate m) -> result<sql::value_t> { return sql::value_t{m}; },
            [](sql::type::datetime_t) -> result<sql::value_t> {
                // TODO(tcs) stringify datetime support
                return stdx::err{error::SQL_TYPE_MISMATCH};
            },
            [&](const auto& v) -> result<sql::value_t> {
                const auto& emplaced{string_pool_.emplace_back(fmt::format("{}", v))};
                return sql::value_t{std::string_view{emplaced}};
            });
    case sql::type::id_t::DATETIME:
        return value.visit(
            [](sql::type::datetime_t v) -> result<sql::value_t> { return sql::value_t{v}; },
            [](const auto&) -> result<sql::value_t> {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            });
    default: return stdx::err{error::SQL_TYPE_MISMATCH};
    }
}

auto expression_evaluator_t::operator()(const sql::binder::literal_expr_t& node, const sql::tuple&)
    -> result<sql::value_t> {
    PROFILE_FUNCTION();
    auto base_val{node.value.visit(
        [&node](stdx::monostate m) {
            if (node.type) { return sql::value_t::make_null(*node.type); }
            return sql::value_t{m};
        },
        [](bool val) { return sql::value_t{val}; },
        [](i64 val) { return sql::value_t{val}; },
        [](f64 val) { return sql::value_t{val}; },
        [](const stdx::fixed::string& val) { return sql::value_t{val.view()}; })};

    return node.type.transform([&](const auto id) { return cast_value(base_val, id); })
        .value_or(std::move(base_val));
}

auto expression_evaluator_t::operator()(const sql::binder::column_ref_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    PROFILE_FUNCTION();
    return input_tuple.get_value(sch_, node.column_idx);
}

namespace {

template <typename T>
[[nodiscard]] auto
evaluate_binary_op(T l, T r, sql::parser::binary_op_t op, sql::type::id_t common_type)
    -> result<sql::value_t> {
    using sql::parser::binary_op_t;
    switch (op) {
    case binary_op_t::ADD: return sql::value_t{static_cast<T>(l + r)};
    case binary_op_t::SUB: return sql::value_t{static_cast<T>(l - r)};
    case binary_op_t::MUL: return sql::value_t{static_cast<T>(l * r)};
    case binary_op_t::DIV: {
        if (r == 0) { return sql::value_t::make_null(common_type); }
        return sql::value_t{static_cast<T>(l / r)};
    }
    case binary_op_t::MOD: {
        if (r == 0) { return sql::value_t::make_null(common_type); }
        if constexpr (std::floating_point<T>) {
            return sql::value_t{std::fmod(l, r)};
        } else {
            return sql::value_t{static_cast<T>(l % r)};
        }
    }
    case binary_op_t::EQ:   return sql::value_t{l == r};
    case binary_op_t::NEQ:  return sql::value_t{l != r};
    case binary_op_t::LT:   return sql::value_t{l < r};
    case binary_op_t::GT:   return sql::value_t{l > r};
    case binary_op_t::LTEQ: return sql::value_t{l <= r};
    case binary_op_t::GTEQ: return sql::value_t{l >= r};
    default:                return stdx::err{error::SQL_TYPE_MISMATCH};
    }
}

} // namespace

auto expression_evaluator_t::operator()(const sql::binder::binary_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    PROFILE_FUNCTION();
    using sql::parser::binary_op_t;

    // Binary boolean operators short circuit
    if (node.op == binary_op_t::AND) {
        auto lhs_val{TRY(evaluate(node.lhs, input_tuple))};
        if (!lhs_val.is_null() && !lhs_val.get_value().as<bool>()) { return sql::value_t{false}; }

        auto rhs_val{TRY(evaluate(node.rhs, input_tuple))};
        if (!rhs_val.is_null() && !rhs_val.get_value().as<bool>()) { return sql::value_t{false}; }

        if (lhs_val.is_null() || rhs_val.is_null()) {
            return sql::value_t::make_null(sql::type::id_t::BOOLEAN);
        }
        return sql::value_t{lhs_val.get_value().as<bool>() && rhs_val.get_value().as<bool>()};
    } else if (node.op == binary_op_t::OR) {
        auto lhs_val{TRY(evaluate(node.lhs, input_tuple))};
        if (!lhs_val.is_null() && lhs_val.get_value().as<bool>()) { return sql::value_t{true}; }

        auto rhs_val{TRY(evaluate(node.rhs, input_tuple))};
        if (!rhs_val.is_null() && !rhs_val.get_value().as<bool>()) { return sql::value_t{true}; }

        if (lhs_val.is_null() || rhs_val.is_null()) {
            return sql::value_t::make_null(sql::type::id_t::BOOLEAN);
        }
        return sql::value_t{lhs_val.get_value().as<bool>() || rhs_val.get_value().as<bool>()};
    }

    const auto lhs_val{TRY(evaluate(node.lhs, input_tuple))};
    const auto rhs_val{TRY(evaluate(node.rhs, input_tuple))};
    if (lhs_val.is_null() || rhs_val.is_null()) {
        if (node.op == binary_op_t::EQ || node.op == binary_op_t::NEQ ||
            node.op == binary_op_t::LT || node.op == binary_op_t::GT ||
            node.op == binary_op_t::LTEQ || node.op == binary_op_t::GTEQ) {
            return sql::value_t::make_null(sql::type::id_t::BOOLEAN);
        }
        auto res_type{sql::type::common_type(lhs_val.type(), rhs_val.type())};
        return sql::value_t::make_null(res_type.value_or(sql::type::id_t::INTEGER));
    }

    const auto common{sql::type::common_type(lhs_val.type(), rhs_val.type())};
    if (!common) { return stdx::err{error::SQL_TYPE_MISMATCH}; }
    const auto l_promoted{TRY(cast_value(lhs_val, *common))};
    const auto r_promoted{TRY(cast_value(rhs_val, *common))};
    ASSERT(l_promoted.get_value().index() == r_promoted.get_value().index());

    auto        op{node.op};
    const auto& l_promoted_val{l_promoted.get_value()};
    const auto& r_promoted_val{r_promoted.get_value()};

    return l_promoted_val.visit(
        [&](bool l) -> result<sql::value_t> {
            auto r{r_promoted_val.as<bool>()};
            switch (op) {
            case binary_op_t::EQ:  return sql::value_t{l == r};
            case binary_op_t::NEQ: return sql::value_t{l != r};
            default:               return stdx::err{error::SQL_TYPE_MISMATCH};
            }
        },
        [&](i8 l) -> result<sql::value_t> {
            return evaluate_binary_op<i8>(l, r_promoted_val.as<i8>(), op, *common);
        },
        [&](i16 l) -> result<sql::value_t> {
            return evaluate_binary_op<i16>(l, r_promoted_val.as<i16>(), op, *common);
        },
        [&](i32 l) -> result<sql::value_t> {
            return evaluate_binary_op<i32>(l, r_promoted_val.as<i32>(), op, *common);
        },
        [&](i64 l) -> result<sql::value_t> {
            return evaluate_binary_op<i64>(l, r_promoted_val.as<i64>(), op, *common);
        },
        [&](f32 l) -> result<sql::value_t> {
            return evaluate_binary_op<f32>(l, r_promoted_val.as<f32>(), op, *common);
        },
        [&](f64 l) -> result<sql::value_t> {
            return evaluate_binary_op<f64>(l, r_promoted_val.as<f64>(), op, *common);
        },
        [&](std::string_view l) -> result<sql::value_t> {
            auto r{r_promoted_val.as<std::string_view>()};
            switch (op) {
            case binary_op_t::EQ:   return sql::value_t{l == r};
            case binary_op_t::NEQ:  return sql::value_t{l != r};
            case binary_op_t::LT:   return sql::value_t{l < r};
            case binary_op_t::GT:   return sql::value_t{l > r};
            case binary_op_t::LTEQ: return sql::value_t{l <= r};
            case binary_op_t::GTEQ: return sql::value_t{l >= r};
            default:                return stdx::err{error::SQL_TYPE_MISMATCH};
            }
        },
        [&](sql::type::datetime_t l) -> result<sql::value_t> {
            auto r{r_promoted_val.as<sql::type::datetime_t>()};
            switch (op) {
            case binary_op_t::EQ: return sql::value_t{l == r};
            default:              return stdx::err{error::SQL_TYPE_MISMATCH};
            }
        },
        [&](const auto&) -> result<sql::value_t> { return stdx::err{error::SQL_TYPE_MISMATCH}; });
}

auto expression_evaluator_t::operator()(const sql::binder::cast_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    PROFILE_FUNCTION();
    auto child_val{TRY(evaluate(node.expr, input_tuple))};
    return cast_value(child_val, node.target_type);
}

namespace {

template <typename T> [[nodiscard]] auto evaluate_minus(T val) -> result<sql::value_t> {
    if constexpr ((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>) {
        return sql::value_t{static_cast<T>(-val)};
    }
    return stdx::err{error::SQL_TYPE_MISMATCH};
}

} // namespace

auto expression_evaluator_t::operator()(const sql::binder::unary_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    PROFILE_FUNCTION();

    auto child_val{TRY(evaluate(node.expr, input_tuple))};
    switch (node.op) {
    case sql::binder::unary_op_t::IS_NULL:     return sql::value_t{child_val.is_null()};
    case sql::binder::unary_op_t::IS_NOT_NULL: return sql::value_t{!child_val.is_null()};
    case sql::binder::unary_op_t::MINUS:       {
        if (child_val.is_null()) { return child_val; }
        return child_val.get_value().visit(
            [](auto v) -> result<sql::value_t> { return evaluate_minus(v); });
    }
    case sql::binder::unary_op_t::NOT: {
        if (child_val.is_null()) { return sql::value_t::make_null(sql::type::id_t::BOOLEAN); }
        return child_val.get_value().visit(
            [](bool v) -> result<sql::value_t> { return sql::value_t{!v}; },
            [](const auto&) -> result<sql::value_t> {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            });
    }
    default: return stdx::err{error::SQL_TYPE_MISMATCH};
    }
}

auto expression_evaluator_t::operator()(const sql::binder::function_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    PROFILE_FUNCTION();
    switch (node.func) {
    case sql::binder::func_type_t::COALESCE:
        for (const auto id : node.args) {
            const auto val{TRY(evaluate(id, input_tuple))};
            if (!val.is_null()) { return val; }
        }

        return node.type.transform([](const auto id) { return sql::value_t::make_null(id); })
            .value_or(sql::value_t{stdx::monostate{}});
    case sql::binder::func_type_t::NULLIF: {
        ASSERT(node.args.size() == 2, "Invariant from binder invalidated");
        auto e1{TRY(evaluate(node.args[0], input_tuple))};
        auto e2{TRY(evaluate(node.args[1], input_tuple))};
        if (e1.is_null() || e2.is_null()) { return e1; }

        auto common{sql::type::common_type(e1.type(), e2.type())};
        if (!common) { return stdx::err{error::SQL_TYPE_MISMATCH}; }
        auto l_promoted{TRY(cast_value(e1, *common))};
        auto l_promoted_val{l_promoted.get_value()};
        auto r_promoted{TRY(cast_value(e2, *common))};
        auto r_promoted_val{r_promoted.get_value()};

        const auto equal{l_promoted_val.visit(
            [&](bool l) { return l == r_promoted_val.as<bool>(); },
            [&](i8 l) { return l == r_promoted_val.as<i8>(); },
            [&](i16 l) { return l == r_promoted_val.as<i16>(); },
            [&](i32 l) { return l == r_promoted_val.as<i32>(); },
            [&](i64 l) { return l == r_promoted_val.as<i64>(); },
            [&](f32 l) { return l == r_promoted_val.as<f32>(); },
            [&](f64 l) { return l == r_promoted_val.as<f64>(); },
            [&](std::string_view l) { return l == r_promoted_val.as<std::string_view>(); },
            [&](sql::type::datetime_t l) {
                return l == r_promoted_val.as<sql::type::datetime_t>();
            },
            [&](const auto&) { return false; })};

        if (equal) {
            return node.type.transform([](const auto id) { return sql::value_t::make_null(id); })
                .value_or(sql::value_t{stdx::monostate{}});
        }
        return e1;
    }
    case sql::binder::func_type_t::LOWER: {
        ASSERT(node.args.size() == 1, "Invariant from binder invalidated");
        auto val{TRY(evaluate(node.args[0], input_tuple))};
        if (val.is_null()) { return val; }
        std::string s{val.get_value().as<std::string_view>()};
        string_utils::inplace_lower(s);
        const auto& emplaced{string_pool_.emplace_back(std::move(s))};
        return sql::value_t{std::string_view{emplaced}};
    }
    case sql::binder::func_type_t::UPPER: {
        ASSERT(node.args.size() == 1, "Invariant from binder invalidated");
        auto val{TRY(evaluate(node.args[0], input_tuple))};
        if (val.is_null()) { return val; }
        std::string s{val.get_value().as<std::string_view>()};
        string_utils::inplace_upper(s);
        const auto& emplaced{string_pool_.emplace_back(std::move(s))};
        return sql::value_t{std::string_view{emplaced}};
    }
    case sql::binder::func_type_t::LENGTH: {
        ASSERT(node.args.size() == 1, "Invariant from binder invalidated");
        const auto val{TRY(evaluate(node.args[0], input_tuple))};
        if (val.is_null()) { return sql::value_t::make_null(sql::type::id_t::BIGINT); }

        const auto sv{val.get_value().as<std::string_view>()};
        return sql::value_t{static_cast<i64>(sv.length())};
    }
    case sql::binder::func_type_t::SUBSTR: {
        ASSERT(node.args.size() == 3, "Invariant from binder invalidated");
        auto val{TRY(evaluate(node.args[0], input_tuple))};
        auto start_val{TRY(evaluate(node.args[1], input_tuple))};
        auto len_val{TRY(evaluate(node.args[2], input_tuple))};

        if (val.is_null() || start_val.is_null() || len_val.is_null()) {
            return sql::value_t::make_null(sql::type::id_t::VARCHAR);
        }

        auto sv{val.get_value().as<std::string_view>()};
        auto start{start_val.get_value().visit([](i8 v) -> i64 { return v; },
                                               [](i16 v) -> i64 { return v; },
                                               [](i32 v) -> i64 { return v; },
                                               [](i64 v) { return v; },
                                               [](const auto&) -> i64 { return 1; })};
        auto len{len_val.get_value().visit([](i8 v) -> i64 { return v; },
                                           [](i16 v) -> i64 { return v; },
                                           [](i32 v) -> i64 { return v; },
                                           [](i64 v) { return v; },
                                           [](const auto&) -> i64 { return 0; })};

        // Empty strings don't need to be heap allocated
        if (len <= 0) { return sql::value_t{std::string_view{""}}; }

        i64 start_idx{start - 1};
        if (start_idx < 0) {
            len += start_idx;
            start_idx = 0;
        }

        if (start_idx >= static_cast<i64>(sv.size()) || len <= 0) {
            return sql::value_t{std::string_view{""}};
        }

        auto substr_len{
            std::min(static_cast<usize>(len), sv.size() - static_cast<usize>(start_idx))};
        std::string s{stdx::string::substr(sv, static_cast<usize>(start_idx), substr_len)};
        const auto& emplaced{string_pool_.emplace_back(std::move(s))};
        return sql::value_t{std::string_view{emplaced}};
    }
    case sql::binder::func_type_t::ABS: {
        ASSERT(node.args.size() == 1, "Invariant from binder invalidated");
        auto val{TRY(evaluate(node.args[0], input_tuple))};
        if (val.is_null()) { return val; }

        return val.get_value().visit([](auto v) -> result<sql::value_t> {
            using T = decltype(v);
            if constexpr ((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>) {
                return sql::value_t{static_cast<T>(std::abs(v))};
            } else {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            }
        });
    }
    case sql::binder::func_type_t::MOD: {
        ASSERT(node.args.size() == 2, "Invariant from binder invalidated");
        const auto e1{TRY(evaluate(node.args[0], input_tuple))};
        const auto e2{TRY(evaluate(node.args[1], input_tuple))};
        if (e1.is_null() || e2.is_null()) {
            const auto common{sql::type::common_type(e1.type(), e2.type())};
            return sql::value_t::make_null(common.value_or(sql::type::id_t::INTEGER));
        }

        const auto common{sql::type::common_type(e1.type(), e2.type())};
        if (!common) { return stdx::err{error::SQL_TYPE_MISMATCH}; }
        const auto  l_promoted{TRY(cast_value(e1, *common))};
        const auto& l_promoted_val{l_promoted.get_value()};
        const auto  r_promoted{TRY(cast_value(e2, *common))};
        const auto& r_promoted_val{r_promoted.get_value()};

        return l_promoted_val.visit([&](auto l) -> result<sql::value_t> {
            using T = decltype(l);
            if constexpr (std::integral<T> || std::floating_point<T>) {
                const auto r{r_promoted_val.as<T>()};
                if (r == 0) { return sql::value_t::make_null(*common); }
                if constexpr (std::floating_point<T>) {
                    return sql::value_t{std::fmod(l, r)};
                } else {
                    return sql::value_t{static_cast<T>(l % r)};
                }
            } else {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            }
        });
    }
    default: return stdx::err{error::SQL_TYPE_MISMATCH};
    }
}

} // namespace cairn::exec
