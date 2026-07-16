#pragma once

#include <limits>
#include <stdx/fixed/string.hh>
#include <type_traits>
#include <utility>
#include <vector>

#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "sql/type.hh"

namespace cairn::sql::ast {

enum class node_kind_t : u8 {
    LITERAL_EXPR,
    IDENTIFIER_EXPR,
    BINARY_EXPR,

    SELECT_STMT,
    CREATE_TABLE_STMT,
    DROP_TABLE_STMT,
    ALTER_TABLE_STMT,
    CREATE_INDEX_STMT,
    DROP_INDEX_STMT,
};

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

using literal_value_t = stdx::variant<stdx::monostate, bool, i64, f64, stdx::fixed::string>;

// Individual node structures (POD)
struct literal_expr_t {
    literal_value_t value;
};

struct identifier_expr_t {
    stdx::fixed::string name;
};

enum class binary_op_t : u8 {
    ADD,
    SUBTRACT,
    MULTIPLY,
    DIVIDE,
    EQUAL,
    NOT_EQUAL,
    LESS_THAN,
    GREATER_THAN,
    LESS_THAN_OR_EQUAL,
    GREATER_THAN_OR_EQUAL,
    AND,
    OR,
};

struct binary_expr_t {
    binary_op_t op;
    node_id_t   lhs;
    node_id_t   rhs;
};

struct select_item_t {
    stdx::option<node_id_t> expr;
};

struct select_stmt_t {
    std::vector<select_item_t> select_list;
    stdx::fixed::string        table_name;
    stdx::option<node_id_t>    where_clause;
};

struct column_def_t {
    stdx::fixed::string name;
    type::id_t          type{type::id_t::INVALID};
    bool                nullable{true};
};

struct create_table_stmt_t {
    stdx::fixed::string       table_name;
    std::vector<column_def_t> column_defs;
};

struct drop_table_stmt_t {
    stdx::fixed::string table_name;
};

struct alter_table_stmt_t {
    stdx::fixed::string table_name;
    column_def_t        column_def;
};

struct create_index_stmt_t {
    stdx::fixed::string              index_name;
    stdx::fixed::string              table_name;
    std::vector<stdx::fixed::string> columns;
};

struct drop_index_stmt_t {
    stdx::fixed::string index_name;
    stdx::fixed::string table_name;
};

using node_data_t = stdx::variant<stdx::monostate,
                                  literal_expr_t,
                                  identifier_expr_t,
                                  binary_expr_t,
                                  select_stmt_t,
                                  create_table_stmt_t,
                                  drop_table_stmt_t,
                                  alter_table_stmt_t,
                                  create_index_stmt_t,
                                  drop_index_stmt_t>;

class ast_t {
  public:
    ast_t()  = default;
    ~ast_t() = default;
    MAKE_MOVE_ONLY(ast_t);

    auto clear() noexcept -> void {
        pool_.clear();
        roots_.clear();
    }

    template <typename T, typename... Args> auto add_node(Args&&... args) -> node_id_t {
        const u64 index{static_cast<u64>(pool_.size())};
        pool_.emplace_back(std::in_place_type<T>, std::forward<Args>(args)...);
        return node_id_t{get_kind<T>(), index};
    }

    auto add_root(node_id_t id) noexcept -> void { roots_.emplace_back(id); }

    [[nodiscard]] auto roots() const noexcept -> gsl::span<const node_id_t> { return roots_; }

    [[nodiscard]] auto operator[](node_id_t id) const noexcept -> const node_data_t& {
        ASSERT(id.is_valid() && id.index() < pool_.size());
        return pool_[id.index()];
    }

    [[nodiscard]] auto operator[](node_id_t id) noexcept -> node_data_t& {
        ASSERT(id.is_valid() && id.index() < pool_.size());
        return pool_[id.index()];
    }

    template <typename T>
    [[nodiscard]] auto get_as_opt(node_id_t id) const noexcept -> stdx::option<const T&> {
        return operator[](id).template as_opt<T>();
    }

  private:
    template <typename T> static constexpr auto get_kind() noexcept -> node_kind_t {
        if constexpr (std::is_same_v<T, literal_expr_t>) {
            return node_kind_t::LITERAL_EXPR;
        } else if constexpr (std::is_same_v<T, identifier_expr_t>) {
            return node_kind_t::IDENTIFIER_EXPR;
        } else if constexpr (std::is_same_v<T, binary_expr_t>) {
            return node_kind_t::BINARY_EXPR;
        } else if constexpr (std::is_same_v<T, select_stmt_t>) {
            return node_kind_t::SELECT_STMT;
        } else if constexpr (std::is_same_v<T, create_table_stmt_t>) {
            return node_kind_t::CREATE_TABLE_STMT;
        } else if constexpr (std::is_same_v<T, drop_table_stmt_t>) {
            return node_kind_t::DROP_TABLE_STMT;
        } else if constexpr (std::is_same_v<T, alter_table_stmt_t>) {
            return node_kind_t::ALTER_TABLE_STMT;
        } else if constexpr (std::is_same_v<T, create_index_stmt_t>) {
            return node_kind_t::CREATE_INDEX_STMT;
        } else if constexpr (std::is_same_v<T, drop_index_stmt_t>) {
            return node_kind_t::DROP_INDEX_STMT;
        } else {
            static_assert(!sizeof(T*), "Unsupported node type");
        }
    }

  private:
    std::vector<node_data_t> pool_;
    std::vector<node_id_t>   roots_;
};

} // namespace cairn::sql::ast

template <> struct stdx::nullable<cairn::sql::ast::node_id_t> {
    using nid_t = cairn::sql::ast::node_id_t;
    [[nodiscard]] static constexpr auto invalid() noexcept -> nid_t {
        return nid_t::make_invalid();
    }

    [[nodiscard]] static constexpr auto is_valid(const nid_t& id) noexcept -> bool {
        return id.is_valid();
    }
};
