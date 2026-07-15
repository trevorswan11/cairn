#pragma once

#include <string>
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

    INVALID,
};

struct node_id_t {
    node_kind_t kind{node_kind_t::INVALID};
    u32         index{0};

    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool {
        return kind != node_kind_t::INVALID;
    }
    [[nodiscard]] static constexpr auto make_invalid() noexcept -> node_id_t { return {}; }
};

using literal_value_t = stdx::variant<stdx::monostate, bool, i64, f64, std::string>;

// Individual node structures (POD)
struct literal_expr_t {
    literal_value_t value;
};

struct identifier_expr_t {
    std::string name;
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
    bool      is_star{false};
    node_id_t expr;
};

struct select_stmt_t {
    std::vector<select_item_t> select_list;
    std::string                table_name;
    node_id_t                  where_clause; // node_id_t::make_invalid() if none
};

struct column_def_t {
    std::string name;
    type::id_t  type{type::id_t::INVALID};
    bool        nullable{true};
};

struct create_table_stmt_t {
    std::string               table_name;
    std::vector<column_def_t> column_defs;
};

struct drop_table_stmt_t {
    std::string table_name;
};

struct alter_table_stmt_t {
    std::string  table_name;
    column_def_t column_def;
};

struct create_index_stmt_t {
    std::string              index_name;
    std::string              table_name;
    std::vector<std::string> columns;
};

struct drop_index_stmt_t {
    std::string index_name;
    std::string table_name;
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

    template <typename T> auto add_node(T&& node) -> node_id_t {
        const u32 index = static_cast<u32>(pool_.size());
        pool_.emplace_back(std::forward<T>(node));
        return node_id_t{get_kind<std::decay_t<T>>(), index};
    }

    auto add_root(node_id_t id) noexcept -> void { roots_.emplace_back(id); }

    [[nodiscard]] auto roots() const noexcept -> gsl::span<const node_id_t> { return roots_; }

    [[nodiscard]] auto operator[](node_id_t id) const noexcept -> const node_data_t& {
        ASSERT(id.is_valid() && id.index < pool_.size());
        return pool_[id.index];
    }

    [[nodiscard]] auto operator[](node_id_t id) noexcept -> node_data_t& {
        ASSERT(id.is_valid() && id.index < pool_.size());
        return pool_[id.index];
    }

    template <typename T>
    [[nodiscard]] auto get_as_opt(node_id_t id) const noexcept -> stdx::option<const T&> {
        return operator[](id).template as_opt<T>();
    }

  private:
    template <typename T> static constexpr auto get_kind() noexcept -> node_kind_t {
        if constexpr (std::is_same_v<T, literal_expr_t>) { return node_kind_t::LITERAL_EXPR; }
        if constexpr (std::is_same_v<T, identifier_expr_t>) { return node_kind_t::IDENTIFIER_EXPR; }
        if constexpr (std::is_same_v<T, binary_expr_t>) { return node_kind_t::BINARY_EXPR; }
        if constexpr (std::is_same_v<T, select_stmt_t>) { return node_kind_t::SELECT_STMT; }
        if constexpr (std::is_same_v<T, create_table_stmt_t>) {
            return node_kind_t::CREATE_TABLE_STMT;
        }
        if constexpr (std::is_same_v<T, drop_table_stmt_t>) { return node_kind_t::DROP_TABLE_STMT; }
        if constexpr (std::is_same_v<T, alter_table_stmt_t>) {
            return node_kind_t::ALTER_TABLE_STMT;
        }
        if constexpr (std::is_same_v<T, create_index_stmt_t>) {
            return node_kind_t::CREATE_INDEX_STMT;
        }
        if constexpr (std::is_same_v<T, drop_index_stmt_t>) { return node_kind_t::DROP_INDEX_STMT; }
        return node_kind_t::INVALID;
    }

  private:
    std::vector<node_data_t> pool_;
    std::vector<node_id_t>   roots_;
};

} // namespace cairn::sql::ast
