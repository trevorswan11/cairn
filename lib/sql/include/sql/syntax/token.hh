#pragma once

#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/diagnostic/location.hh"

namespace cairn {

namespace sql::syntax {

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
    COMMENT, // single or multiline

    ILLEGAL,
};

[[nodiscard]] auto misc_type_from_char(char c) noexcept -> stdx::option<token_type_t>;

[[nodiscard]] auto max_operator_length() noexcept -> usize;
[[nodiscard]] auto get_operator_opt(std::string_view sv) noexcept -> stdx::option<token_type_t>;
[[nodiscard]] auto get_keyword_opt(std::string_view sv) noexcept -> stdx::option<token_type_t>;

struct token_t {
    token_type_t     type;
    std::string_view lexeme;
    location         loc;

    constexpr token_t() noexcept = default;
    constexpr token_t(token_type_t tt, std::string_view lexeme) noexcept
        : type{tt}, lexeme{lexeme} {}
    constexpr token_t(token_type_t tt, std::string_view slice, usize line, usize column) noexcept
        : type{tt}, lexeme{slice}, loc{line, column} {}

    [[nodiscard]] auto           is_valid_ident() const noexcept -> bool;
    [[nodiscard]] constexpr auto operator==(const token_t&) const noexcept -> bool = default;
};

} // namespace sql::syntax

template <> struct source_info<sql::syntax::token_t> {
    static auto get(const sql::syntax::token_t& tok) -> location { return tok.loc; }
};

} // namespace cairn
