#pragma once

#include <limits>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::sql {

namespace parser {

struct literal_expr_t;
struct identifier_expr_t;
struct binary_expr_t;
struct aggregate_expr_t;
struct unary_expr_t;
struct function_expr_t;
struct select_stmt_t;
struct create_table_stmt_t;
struct drop_table_stmt_t;
struct alter_table_stmt_t;
struct create_index_stmt_t;
struct drop_index_stmt_t;

} // namespace parser

namespace binder {

struct literal_expr_t;
struct column_ref_expr_t;
struct binary_expr_t;
struct aggregate_expr_t;
struct cast_expr_t;
struct unary_expr_t;
struct function_expr_t;
struct select_stmt_t;
struct create_table_stmt_t;
struct drop_table_stmt_t;
struct alter_table_stmt_t;
struct create_index_stmt_t;
struct drop_index_stmt_t;

} // namespace binder

namespace detail {

enum class node_kind_t : u8 {
    LITERAL_EXPR,
    COLUMN_REF_EXPR,
    IDENTIFIER_EXPR,
    BINARY_EXPR,
    AGGREGATE_EXPR,
    CAST_EXPR,
    UNARY_EXPR,
    FUNCTION_EXPR,

    SELECT_STMT,
    CREATE_TABLE_STMT,
    DROP_TABLE_STMT,
    ALTER_TABLE_STMT,
    CREATE_INDEX_STMT,
    DROP_INDEX_STMT,
};

template <typename T> constexpr auto node_kind_of() noexcept -> node_kind_t {
    if constexpr (std::is_same_v<T, binder::literal_expr_t>) {
        return node_kind_t::LITERAL_EXPR;
    } else if constexpr (std::is_same_v<T, binder::column_ref_expr_t>) {
        return node_kind_t::COLUMN_REF_EXPR;
    } else if constexpr (std::is_same_v<T, binder::binary_expr_t>) {
        return node_kind_t::BINARY_EXPR;
    } else if constexpr (std::is_same_v<T, binder::aggregate_expr_t>) {
        return node_kind_t::AGGREGATE_EXPR;
    } else if constexpr (std::is_same_v<T, binder::cast_expr_t>) {
        return node_kind_t::CAST_EXPR;
    } else if constexpr (std::is_same_v<T, binder::unary_expr_t>) {
        return node_kind_t::UNARY_EXPR;
    } else if constexpr (std::is_same_v<T, binder::function_expr_t>) {
        return node_kind_t::FUNCTION_EXPR;
    } else if constexpr (std::is_same_v<T, parser::unary_expr_t>) {
        return node_kind_t::UNARY_EXPR;
    } else if constexpr (std::is_same_v<T, parser::function_expr_t>) {
        return node_kind_t::FUNCTION_EXPR;
    } else if constexpr (std::is_same_v<T, binder::select_stmt_t>) {
        return node_kind_t::SELECT_STMT;
    } else if constexpr (std::is_same_v<T, binder::create_table_stmt_t>) {
        return node_kind_t::CREATE_TABLE_STMT;
    } else if constexpr (std::is_same_v<T, binder::drop_table_stmt_t>) {
        return node_kind_t::DROP_TABLE_STMT;
    } else if constexpr (std::is_same_v<T, binder::alter_table_stmt_t>) {
        return node_kind_t::ALTER_TABLE_STMT;
    } else if constexpr (std::is_same_v<T, binder::create_index_stmt_t>) {
        return node_kind_t::CREATE_INDEX_STMT;
    } else if constexpr (std::is_same_v<T, binder::drop_index_stmt_t>) {
        return node_kind_t::DROP_INDEX_STMT;
    } else if constexpr (std::is_same_v<T, parser::literal_expr_t>) {
        return node_kind_t::LITERAL_EXPR;
    } else if constexpr (std::is_same_v<T, parser::identifier_expr_t>) {
        return node_kind_t::IDENTIFIER_EXPR;
    } else if constexpr (std::is_same_v<T, parser::binary_expr_t>) {
        return node_kind_t::BINARY_EXPR;
    } else if constexpr (std::is_same_v<T, parser::aggregate_expr_t>) {
        return node_kind_t::AGGREGATE_EXPR;
    } else if constexpr (std::is_same_v<T, parser::select_stmt_t>) {
        return node_kind_t::SELECT_STMT;
    } else if constexpr (std::is_same_v<T, parser::create_table_stmt_t>) {
        return node_kind_t::CREATE_TABLE_STMT;
    } else if constexpr (std::is_same_v<T, parser::drop_table_stmt_t>) {
        return node_kind_t::DROP_TABLE_STMT;
    } else if constexpr (std::is_same_v<T, parser::alter_table_stmt_t>) {
        return node_kind_t::ALTER_TABLE_STMT;
    } else if constexpr (std::is_same_v<T, parser::create_index_stmt_t>) {
        return node_kind_t::CREATE_INDEX_STMT;
    } else if constexpr (std::is_same_v<T, parser::drop_index_stmt_t>) {
        return node_kind_t::DROP_INDEX_STMT;
    } else {
        static_assert(!sizeof(T*), "Unsupported node type");
    }
}

class node_id_t {
  public:
    constexpr node_id_t() noexcept : raw_{INVALID_ID} {}

    constexpr node_id_t(node_kind_t kind, u64 index) noexcept : raw_{} {
        ASSERT(index <= INDEX_MASK, "Requested node index is too large");
        raw_ |= static_cast<u64>(kind) << KIND_OFFSET;
        raw_ |= index;
    }

    [[nodiscard]] constexpr auto kind() const noexcept -> node_kind_t {
        return static_cast<node_kind_t>((raw_ & KIND_MASK) >> KIND_OFFSET);
    }

    [[nodiscard]] constexpr auto index() const noexcept -> usize {
        return static_cast<usize>(raw_ & INDEX_MASK);
    }

    [[nodiscard]] static constexpr auto make_invalid() noexcept -> node_id_t {
        return node_id_t{INVALID_ID};
    }

    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return raw_ != INVALID_ID; }

    [[nodiscard]] friend auto operator<=>(const node_id_t&, const node_id_t&) noexcept = default;
    [[nodiscard]] friend auto operator==(const node_id_t&, const node_id_t&) noexcept
        -> bool = default;

  private:
    constexpr explicit node_id_t(u64 raw) noexcept : raw_{raw} {}

  private:
    static constexpr u64 KIND_MASK{0xFF00000000000000ULL};
    static constexpr u64 KIND_OFFSET{56};
    static constexpr u64 INDEX_MASK{0x00FFFFFFFFFFFFFFULL};
    static constexpr u64 INVALID_ID{std::numeric_limits<u64>::max()};

  private:
    u64 raw_;
};

} // namespace detail

} // namespace cairn::sql

template <> struct stdx::nullable<cairn::sql::detail::node_id_t> {
    using nid_t = cairn::sql::detail::node_id_t;
    [[nodiscard]] static constexpr auto invalid() noexcept -> nid_t {
        return nid_t::make_invalid();
    }

    [[nodiscard]] static constexpr auto is_valid(const nid_t& id) noexcept -> bool {
        return id.is_valid();
    }
};
