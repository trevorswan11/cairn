#pragma once

#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::sql::syntax {

enum class token_type_t : u8 {
    END_OF_FILE,

    IDENTIFIER,      // can be surrounded by backticks
    INTEGER_LITERAL, // tiny/small/int/big
    FLOAT_LITERAL,   // float/double
    STRING_LITERAL,  // varchar

    SELECT,
    FROM,
    WHERE,
    CREATE,
    DROP,
    ALTER,
    TABLE,
    INDEX,
    ADD,
    COLUMN,
    LEFT,
    JOIN,
    GROUP,
    BY,
    INNER,
    DELETE,

    BOOLEAN,
    TINYINT,
    SMALLINT,
    INTEGER,
    BIGINT,
    FLOAT,
    DOUBLE,
    VARCHAR,
    DATETIME,

    KW_TRUE,
    KW_FALSE,
    KW_NULL,
    KW_AND,
    KW_OR,

    PLUS,
    MINUS,
    STAR,
    SLASH,

    EQ,
    NEQ,
    LT,
    LT_EQ,
    GT,
    GT_EQ,

    COMMA,
    DOT,
    SEMICOLON,
    LPAREN,
    RPAREN,
    COMMENT, // # or --

    INVALID,
};

[[nodiscard]] auto max_operator_length() noexcept -> usize;
[[nodiscard]] auto get_operator_opt(std::string_view sv) noexcept -> stdx::option<token_type_t>;
[[nodiscard]] auto get_keyword_opt(std::string_view sv) noexcept -> stdx::option<token_type_t>;

} // namespace cairn::sql::syntax
