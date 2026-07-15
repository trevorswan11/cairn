#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "sql/type.hh"

namespace cairn::sql::ast {

class ast_node_t {
  public:
    constexpr ast_node_t() noexcept = default;
    virtual ~ast_node_t()           = default;
    MAKE_MOVE_ONLY(ast_node_t);
};

class expr_node_t : public ast_node_t {};

using literal_value_t = stdx::variant<stdx::monostate, bool, i64, f64, std::string>;

class literal_expr_t final : public expr_node_t {
  public:
    explicit literal_expr_t(literal_value_t val) noexcept : value_{std::move(val)} {}

    [[nodiscard]] auto value() const noexcept -> const literal_value_t& { return value_; }

  private:
    literal_value_t value_;
};

class identifier_expr_t final : public expr_node_t {
  public:
    explicit identifier_expr_t(std::string name) noexcept : name_{std::move(name)} {}

    [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  private:
    std::string name_;
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

class binary_expr_t final : public expr_node_t {
  public:
    binary_expr_t(binary_op_t op, stdx::box<expr_node_t> lhs, stdx::box<expr_node_t> rhs) noexcept
        : op_{op}, lhs_{std::move(lhs)}, rhs_{std::move(rhs)} {}

    [[nodiscard]] auto op() const noexcept -> binary_op_t { return op_; }
    [[nodiscard]] auto lhs() const noexcept -> const expr_node_t& { return *lhs_; }
    [[nodiscard]] auto rhs() const noexcept -> const expr_node_t& { return *rhs_; }

  private:
    binary_op_t            op_;
    stdx::box<expr_node_t> lhs_;
    stdx::box<expr_node_t> rhs_;
};

class stmt_node_t : public ast_node_t {};

struct select_item_t {
    bool                   is_star{false};
    stdx::box<expr_node_t> expr;
};

class select_stmt_t final : public stmt_node_t {
  public:
    select_stmt_t(std::vector<select_item_t>      select_list,
                  std::string                     table_name,
                  stdx::nullable_box<expr_node_t> where_clause = nullptr) noexcept
        : select_list_{std::move(select_list)}, table_name_{std::move(table_name)},
          where_clause_{std::move(where_clause)} {}

    [[nodiscard]] auto select_list() const noexcept -> gsl::span<const select_item_t> {
        return select_list_;
    }
    [[nodiscard]] auto table_name() const noexcept -> std::string_view { return table_name_; }
    [[nodiscard]] auto where_clause() const noexcept -> stdx::option<const expr_node_t&> {
        return where_clause_.get();
    }

  private:
    std::vector<select_item_t>      select_list_;
    std::string                     table_name_;
    stdx::nullable_box<expr_node_t> where_clause_;
};

struct column_def_t {
    std::string name;
    type::id_t  type{type::id_t::INVALID};
    bool        nullable{true};
};

class create_table_stmt_t final : public stmt_node_t {
  public:
    create_table_stmt_t(std::string table_name, std::vector<column_def_t> column_defs) noexcept
        : table_name_{std::move(table_name)}, column_defs_{std::move(column_defs)} {}

    [[nodiscard]] auto table_name() const noexcept -> std::string_view { return table_name_; }
    [[nodiscard]] auto column_defs() const noexcept -> gsl::span<const column_def_t> {
        return column_defs_;
    }

  private:
    std::string               table_name_;
    std::vector<column_def_t> column_defs_;
};

class drop_table_stmt_t final : public stmt_node_t {
  public:
    explicit drop_table_stmt_t(std::string table_name) noexcept
        : table_name_{std::move(table_name)} {}

    [[nodiscard]] auto table_name() const noexcept -> std::string_view { return table_name_; }

  private:
    std::string table_name_;
};

class alter_table_stmt_t final : public stmt_node_t {
  public:
    alter_table_stmt_t(std::string table_name, column_def_t column_def) noexcept
        : table_name_{std::move(table_name)}, column_def_{std::move(column_def)} {}

    [[nodiscard]] auto table_name() const noexcept -> std::string_view { return table_name_; }
    [[nodiscard]] auto column_def() const noexcept -> const column_def_t& { return column_def_; }

  private:
    std::string  table_name_;
    column_def_t column_def_;
};

class create_index_stmt_t final : public stmt_node_t {
  public:
    create_index_stmt_t(std::string              index_name,
                        std::string              table_name,
                        std::vector<std::string> columns) noexcept
        : index_name_{std::move(index_name)}, table_name_{std::move(table_name)},
          columns_{std::move(columns)} {}

    [[nodiscard]] auto index_name() const noexcept -> std::string_view { return index_name_; }
    [[nodiscard]] auto table_name() const noexcept -> std::string_view { return table_name_; }
    [[nodiscard]] auto columns() const noexcept -> gsl::span<const std::string> { return columns_; }

  private:
    std::string              index_name_;
    std::string              table_name_;
    std::vector<std::string> columns_;
};

class drop_index_stmt_t final : public stmt_node_t {
  public:
    drop_index_stmt_t(std::string index_name, std::string table_name) noexcept
        : index_name_{std::move(index_name)}, table_name_{std::move(table_name)} {}

    [[nodiscard]] auto index_name() const noexcept -> std::string_view { return index_name_; }
    [[nodiscard]] auto table_name() const noexcept -> std::string_view { return table_name_; }

  private:
    std::string index_name_;
    std::string table_name_;
};

} // namespace cairn::sql::ast
