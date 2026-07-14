#include <initializer_list>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "sql/syntax/lexer.hh"
#include "sql/syntax/token.hh"

namespace cairn::tests {

using namespace cairn::sql;
using syntax::token_type_t;
using expected_lexeme = std::pair<token_type_t, std::string_view>;

namespace {

auto test_lexer(std::string_view input, std::initializer_list<expected_lexeme> expecteds) -> void {
    syntax::lexer_t lexer{input};
    for (const auto& [expected_tok, expected_slice] : expecteds) {
        const auto token{lexer.advance()};
        CHECK(expected_tok == token.type);
        CHECK(expected_slice == token.lexeme);
    }

    // Should be true regardless of caller putting END_OF_FILE in their list
    const auto& end_tok{lexer.advance()};
    CHECK(end_tok.type == token_type_t::END_OF_FILE);
    CHECK(end_tok.lexeme == "");
}

} // namespace

TEST_CASE("Lexing invalid characters") {
    syntax::lexer_t lexer{"月😭🎶"};
    const auto      tokens{lexer.consume()};

    for (usize i{0}; i < tokens.size(); ++i) {
        const auto& token{tokens[i]};
        if (i == tokens.size() - 1) {
            CHECK(token.type == token_type_t::END_OF_FILE);
            break;
        }
        CHECK(token.type == token_type_t::ILLEGAL);
    }
}

TEST_CASE("lexer_t over-consumption") {
    syntax::lexer_t lexer{"SELECT"};
    CHECK(lexer.consume().size() == 2); // SELECT and END_OF_FILE
    for (usize i{0}; i < 100; ++i) { CHECK(lexer.advance().type == token_type_t::END_OF_FILE); }
}

TEST_CASE("Lexing symbols and operators") {
    test_lexer("= != <> < > <= >= + - * / , . ; ( )",
               {
                   {token_type_t::EQ, "="},
                   {token_type_t::NEQ, "!="},
                   {token_type_t::NEQ, "<>"},
                   {token_type_t::LT, "<"},
                   {token_type_t::GT, ">"},
                   {token_type_t::LT_EQ, "<="},
                   {token_type_t::GT_EQ, ">="},
                   {token_type_t::PLUS, "+"},
                   {token_type_t::MINUS, "-"},
                   {token_type_t::STAR, "*"},
                   {token_type_t::SLASH, "/"},
                   {token_type_t::COMMA, ","},
                   {token_type_t::DOT, "."},
                   {token_type_t::SEMICOLON, ";"},
                   {token_type_t::LPAREN, "("},
                   {token_type_t::RPAREN, ")"},
               });
}

TEST_CASE("Lexing basic SQL query snippet") {
    test_lexer("SELECT id, name, age FROM users WHERE age >= 21;",
               {
                   {token_type_t::SELECT, "SELECT"},
                   {token_type_t::IDENTIFIER, "id"},
                   {token_type_t::COMMA, ","},
                   {token_type_t::IDENTIFIER, "name"},
                   {token_type_t::COMMA, ","},
                   {token_type_t::IDENTIFIER, "age"},
                   {token_type_t::FROM, "FROM"},
                   {token_type_t::IDENTIFIER, "users"},
                   {token_type_t::WHERE, "WHERE"},
                   {token_type_t::IDENTIFIER, "age"},
                   {token_type_t::GT_EQ, ">="},
                   {token_type_t::INTEGER_LITERAL, "21"},
                   {token_type_t::SEMICOLON, ";"},
               });
}

TEST_CASE("Lexing basic SQL DDL snippet") {
    test_lexer("CREATE TABLE users (id INTEGER, name VARCHAR, created_at DATETIME);",
               {
                   {token_type_t::CREATE, "CREATE"},
                   {token_type_t::TABLE, "TABLE"},
                   {token_type_t::IDENTIFIER, "users"},
                   {token_type_t::LPAREN, "("},
                   {token_type_t::IDENTIFIER, "id"},
                   {token_type_t::INTEGER, "INTEGER"},
                   {token_type_t::COMMA, ","},
                   {token_type_t::IDENTIFIER, "name"},
                   {token_type_t::VARCHAR, "VARCHAR"},
                   {token_type_t::COMMA, ","},
                   {token_type_t::IDENTIFIER, "created_at"},
                   {token_type_t::DATETIME, "DATETIME"},
                   {token_type_t::RPAREN, ")"},
                   {token_type_t::SEMICOLON, ";"},
               });
}

TEST_CASE("Lexing numbers") {
    test_lexer("0 123 3.14 42.0 1e20 1.e-3 0.5",
               {
                   {token_type_t::INTEGER_LITERAL, "0"},
                   {token_type_t::INTEGER_LITERAL, "123"},
                   {token_type_t::FLOAT_LITERAL, "3.14"},
                   {token_type_t::FLOAT_LITERAL, "42.0"},
                   {token_type_t::FLOAT_LITERAL, "1e20"},
                   {token_type_t::FLOAT_LITERAL, "1.e-3"},
                   {token_type_t::FLOAT_LITERAL, "0.5"},
               });
}

TEST_CASE("Lexing keywords case-insensitively") {
    test_lexer("select From WhErE CrEaTe drop alter table index add column boolean tinyint "
               "smallint int integer bigint float double varchar datetime null true false",
               {
                   {token_type_t::SELECT, "select"},     {token_type_t::FROM, "From"},
                   {token_type_t::WHERE, "WhErE"},       {token_type_t::CREATE, "CrEaTe"},
                   {token_type_t::DROP, "drop"},         {token_type_t::ALTER, "alter"},
                   {token_type_t::TABLE, "table"},       {token_type_t::INDEX, "index"},
                   {token_type_t::ADD, "add"},           {token_type_t::COLUMN, "column"},
                   {token_type_t::BOOLEAN, "boolean"},   {token_type_t::TINYINT, "tinyint"},
                   {token_type_t::SMALLINT, "smallint"}, {token_type_t::INTEGER, "int"},
                   {token_type_t::INTEGER, "integer"},   {token_type_t::BIGINT, "bigint"},
                   {token_type_t::FLOAT, "float"},       {token_type_t::DOUBLE, "double"},
                   {token_type_t::VARCHAR, "varchar"},   {token_type_t::DATETIME, "datetime"},
                   {token_type_t::KW_NULL, "null"},      {token_type_t::KW_TRUE, "true"},
                   {token_type_t::KW_FALSE, "false"},
               });
}

TEST_CASE("Lexing comments") {
    test_lexer("SELECT id FROM users; -- this is a comment\n"
               "SELECT name FROM users; /* this is a\n"
               "multi-line comment */ SELECT age FROM users;",
               {
                   {token_type_t::SELECT, "SELECT"},
                   {token_type_t::IDENTIFIER, "id"},
                   {token_type_t::FROM, "FROM"},
                   {token_type_t::IDENTIFIER, "users"},
                   {token_type_t::SEMICOLON, ";"},
                   {token_type_t::COMMENT, "-- this is a comment"},
                   {token_type_t::SELECT, "SELECT"},
                   {token_type_t::IDENTIFIER, "name"},
                   {token_type_t::FROM, "FROM"},
                   {token_type_t::IDENTIFIER, "users"},
                   {token_type_t::SEMICOLON, ";"},
                   {token_type_t::COMMENT, "/* this is a\nmulti-line comment */"},
                   {token_type_t::SELECT, "SELECT"},
                   {token_type_t::IDENTIFIER, "age"},
                   {token_type_t::FROM, "FROM"},
                   {token_type_t::IDENTIFIER, "users"},
                   {token_type_t::SEMICOLON, ";"},
               });
}

TEST_CASE("Lexing string literals") {
    test_lexer("'This is a string' 'Hello, World!' 'Hello\\n, World!' 'unterminated",
               {
                   {token_type_t::STRING_LITERAL, "'This is a string'"},
                   {token_type_t::STRING_LITERAL, "'Hello, World!'"},
                   {token_type_t::STRING_LITERAL, "'Hello\\n, World!'"},
                   {token_type_t::ILLEGAL, "'unterminated"},
               });
}

TEST_CASE("Lexing quoted and backticked identifiers") {
    test_lexer("`users` \"users\"",
               {
                   {token_type_t::IDENTIFIER, "`users`"},
                   {token_type_t::IDENTIFIER, "\"users\""},
               });
}

TEST_CASE("Lexer snapshot and restore") {
    syntax::lexer_t lexer{"SELECT id, name, age FROM users;"};
    const auto      t1{lexer.advance()};
    CHECK(t1.type == token_type_t::SELECT);

    const auto snap{lexer.snapshot()};
    const auto t2{lexer.advance()};
    CHECK(t2.type == token_type_t::IDENTIFIER);
    CHECK(t2.lexeme == "id");

    const auto t3{lexer.advance()};
    CHECK(t3.type == token_type_t::COMMA);

    const auto t4{lexer.advance()};
    CHECK(t4.type == token_type_t::IDENTIFIER);
    CHECK(t4.lexeme == "name");

    lexer.restore(snap);
    const auto t2_restored{lexer.advance()};
    CHECK(t2_restored.type == token_type_t::IDENTIFIER);
    CHECK(t2_restored.lexeme == "id");
    CHECK(t2_restored.loc == t2.loc);

    const auto t3_restored{lexer.advance()};
    CHECK(t3_restored.type == token_type_t::COMMA);

    const auto t4_restored{lexer.advance()};
    CHECK(t4_restored.type == token_type_t::IDENTIFIER);
    CHECK(t4_restored.lexeme == "name");
}

} // namespace cairn::tests
