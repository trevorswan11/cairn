#include "exec/expression_evaluator.hh"

#include <charconv>
#include <cmath>
#include <concepts>
#include <string_view>

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "sql/binder/nodes.hh"
#include "sql/tuple.hh"
#include "sql/type.hh"
#include "sql/value.hh"
#include "stdx/variant.hh"
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
    case sql::type::id_t::TINYINT:
        return value.visit(
            [](bool v) -> result<sql::value_t> { return sql::value_t{static_cast<i8>(v ? 1 : 0)}; },
            [](i8 v) -> result<sql::value_t> { return sql::value_t{v}; },
            [](i16 v) -> result<sql::value_t> { return sql::value_t{static_cast<i8>(v)}; },
            [](i32 v) -> result<sql::value_t> { return sql::value_t{static_cast<i8>(v)}; },
            [](i64 v) -> result<sql::value_t> { return sql::value_t{static_cast<i8>(v)}; },
            [](f32 v) -> result<sql::value_t> { return sql::value_t{static_cast<i8>(v)}; },
            [](f64 v) -> result<sql::value_t> { return sql::value_t{static_cast<i8>(v)}; },
            [](std::string_view v) -> result<sql::value_t> {
                i64        val;
                const auto res{std::from_chars(v.begin(), v.end(), val)};
                if (res.ec != std::errc{} || res.ptr != v.end()) {
                    return stdx::err{error::SQL_TYPE_MISMATCH};
                }
                return sql::value_t{static_cast<i8>(val)};
            },
            [](const auto&) -> result<sql::value_t> {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            });
    case sql::type::id_t::SMALLINT:
        return value.visit(
            [](bool v) -> result<sql::value_t> {
                return sql::value_t{static_cast<i16>(v ? 1 : 0)};
            },
            [](i8 v) -> result<sql::value_t> { return sql::value_t{static_cast<i16>(v)}; },
            [](i16 v) -> result<sql::value_t> { return sql::value_t{v}; },
            [](i32 v) -> result<sql::value_t> { return sql::value_t{static_cast<i16>(v)}; },
            [](i64 v) -> result<sql::value_t> { return sql::value_t{static_cast<i16>(v)}; },
            [](f32 v) -> result<sql::value_t> { return sql::value_t{static_cast<i16>(v)}; },
            [](f64 v) -> result<sql::value_t> { return sql::value_t{static_cast<i16>(v)}; },
            [](std::string_view v) -> result<sql::value_t> {
                i64        val;
                const auto res{std::from_chars(v.begin(), v.end(), val)};
                if (res.ec != std::errc{} || res.ptr != v.end()) {
                    return stdx::err{error::SQL_TYPE_MISMATCH};
                }
                return sql::value_t{static_cast<i16>(val)};
            },
            [](const auto&) -> result<sql::value_t> {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            });
    case sql::type::id_t::INTEGER:
        return value.visit(
            [](bool v) -> result<sql::value_t> {
                return sql::value_t{static_cast<i32>(v ? 1 : 0)};
            },
            [](i8 v) -> result<sql::value_t> { return sql::value_t{static_cast<i32>(v)}; },
            [](i16 v) -> result<sql::value_t> { return sql::value_t{static_cast<i32>(v)}; },
            [](i32 v) -> result<sql::value_t> { return sql::value_t{v}; },
            [](i64 v) -> result<sql::value_t> { return sql::value_t{static_cast<i32>(v)}; },
            [](f32 v) -> result<sql::value_t> { return sql::value_t{static_cast<i32>(v)}; },
            [](f64 v) -> result<sql::value_t> { return sql::value_t{static_cast<i32>(v)}; },
            [](std::string_view v) -> result<sql::value_t> {
                i64        val;
                const auto res{std::from_chars(v.begin(), v.end(), val)};
                if (res.ec != std::errc{} || res.ptr != v.end()) {
                    return stdx::err{error::SQL_TYPE_MISMATCH};
                }
                return sql::value_t{static_cast<i32>(val)};
            },
            [](const auto&) -> result<sql::value_t> {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            });
    case sql::type::id_t::BIGINT:
        return value.visit(
            [](bool v) -> result<sql::value_t> {
                return sql::value_t{static_cast<i64>(v ? 1 : 0)};
            },
            [](i8 v) -> result<sql::value_t> { return sql::value_t{static_cast<i64>(v)}; },
            [](i16 v) -> result<sql::value_t> { return sql::value_t{static_cast<i64>(v)}; },
            [](i32 v) -> result<sql::value_t> { return sql::value_t{static_cast<i64>(v)}; },
            [](i64 v) -> result<sql::value_t> { return sql::value_t{v}; },
            [](f32 v) -> result<sql::value_t> { return sql::value_t{static_cast<i64>(v)}; },
            [](f64 v) -> result<sql::value_t> { return sql::value_t{static_cast<i64>(v)}; },
            [](std::string_view v) -> result<sql::value_t> {
                i64        val;
                const auto res{std::from_chars(v.begin(), v.end(), val)};
                if (res.ec != std::errc{} || res.ptr != v.end()) {
                    return stdx::err{error::SQL_TYPE_MISMATCH};
                }
                return sql::value_t{val};
            },
            [](const auto&) -> result<sql::value_t> {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            });
    case sql::type::id_t::FLOAT:
        return value.visit(
            [](bool v) -> result<sql::value_t> {
                return sql::value_t{static_cast<f32>(v ? 1 : 0)};
            },
            [](i8 v) -> result<sql::value_t> { return sql::value_t{static_cast<f32>(v)}; },
            [](i16 v) -> result<sql::value_t> { return sql::value_t{static_cast<f32>(v)}; },
            [](i32 v) -> result<sql::value_t> { return sql::value_t{static_cast<f32>(v)}; },
            [](i64 v) -> result<sql::value_t> { return sql::value_t{static_cast<f32>(v)}; },
            [](f32 v) -> result<sql::value_t> { return sql::value_t{v}; },
            [](f64 v) -> result<sql::value_t> { return sql::value_t{static_cast<f32>(v)}; },
            [](std::string_view v) -> result<sql::value_t> {
                f64        val;
                const auto res{std::from_chars(v.begin(), v.end(), val)};
                if (res.ec != std::errc{} || res.ptr != v.end()) {
                    return stdx::err{error::SQL_TYPE_MISMATCH};
                }
                return sql::value_t{static_cast<f32>(val)};
            },
            [](const auto&) -> result<sql::value_t> {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            });
    case sql::type::id_t::DOUBLE:
        return value.visit(
            [](bool v) -> result<sql::value_t> {
                return sql::value_t{static_cast<f64>(v ? 1 : 0)};
            },
            [](i8 v) -> result<sql::value_t> { return sql::value_t{static_cast<f64>(v)}; },
            [](i16 v) -> result<sql::value_t> { return sql::value_t{static_cast<f64>(v)}; },
            [](i32 v) -> result<sql::value_t> { return sql::value_t{static_cast<f64>(v)}; },
            [](i64 v) -> result<sql::value_t> { return sql::value_t{static_cast<f64>(v)}; },
            [](f32 v) -> result<sql::value_t> { return sql::value_t{static_cast<f64>(v)}; },
            [](f64 v) -> result<sql::value_t> { return sql::value_t{v}; },
            [](std::string_view v) -> result<sql::value_t> {
                f64        val;
                const auto res{std::from_chars(v.begin(), v.end(), val)};
                if (res.ec != std::errc{} || res.ptr != v.end()) {
                    return stdx::err{error::SQL_TYPE_MISMATCH};
                }
                return sql::value_t{val};
            },
            [](const auto&) -> result<sql::value_t> {
                return stdx::err{error::SQL_TYPE_MISMATCH};
            });
    case sql::type::id_t::VARCHAR:
        return value.visit(
            [](sql::type::datetime_t) -> result<sql::value_t> {
                // TODO(tcs) stringify datetime support
                return stdx::err{error::SQL_TYPE_MISMATCH};
            },
            [&](const auto& v) -> result<sql::value_t> {
                auto& emplaced{string_pool_.emplace_back(fmt::format("{}", v))};
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

auto expression_evaluator_t::operator()(sql::binder::node_id_t id,
                                        const stdx::monostate& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t             id,
                                        const sql::binder::literal_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t                id,
                                        const sql::binder::column_ref_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t            id,
                                        const sql::binder::binary_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t          id,
                                        const sql::binder::cast_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t           id,
                                        const sql::binder::unary_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

auto expression_evaluator_t::operator()(sql::binder::node_id_t              id,
                                        const sql::binder::function_expr_t& node,
                                        const sql::tuple& input_tuple) -> result<sql::value_t> {
    TODO(id, node, input_tuple);
}

} // namespace cairn::exec
