#include "sql/syntax/lexer.hh"

#include <cctype>
#include <string_view>
#include <vector>

#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "sql/syntax/token.hh"

namespace cairn::sql::syntax {

auto lexer_t::reset(std::string_view input) noexcept -> void { *this = lexer_t{input}; }

auto lexer_t::advance() noexcept -> token_t {
    PROFILE_FUNCTION();
    skip_whitespace();

    token_t    token{{}, {}, cursor_.line_no_, cursor_.col_no_};
    const auto maybe_operator{read_operator()};

    if (maybe_operator) {
        if (maybe_operator->type == token_type_t::END_OF_FILE) { return *maybe_operator; }
        for (usize i{0}; i < maybe_operator->lexeme.size(); ++i) { read_character(); }
        if (maybe_operator->type == token_type_t::COMMENT) { return read_comment(); }
        return *maybe_operator;
    }

    const auto maybe_misc_token_type{misc_type_from_char(cursor_.current_byte_)};
    if (maybe_misc_token_type) {
        token.lexeme = stdx::string::substr(input_, cursor_.pos_, 1);
        token.type   = *maybe_misc_token_type;
    } else if (std::isalpha(cursor_.current_byte_)) {
        token.lexeme = read_ident();
        token.type   = lu_ident(token.lexeme);
        return token;
    } else if (std::isdigit(cursor_.current_byte_)) {
        return read_number();
    } else if (cursor_.current_byte_ == '\'') {
        return read_string();
    } else {
        token.lexeme = stdx::string::substr(input_, cursor_.pos_, 1);
        token.type   = token_type_t::ILLEGAL;
    }

    read_character();
    return token;
}

auto lexer_t::consume() -> std::vector<token_t> {
    reset(input_);

    std::vector<token_t> tokens;
    do { tokens.emplace_back(advance()); } while (tokens.back().type != token_type_t::END_OF_FILE);

    return tokens;
}

auto lexer_t::skip_whitespace() noexcept -> void {
    while (std::isspace(cursor_.current_byte_)) { read_character(); }
}

auto lexer_t::lu_ident(std::string_view ident) noexcept -> token_type_t {
    return get_keyword_opt(ident).value_or(token_type_t::IDENTIFIER);
}

auto lexer_t::read_character(u8 n) noexcept -> void {
    for (u8 i{0}; i < n; ++i) {
        if (cursor_.peek_pos_ >= input_.size()) {
            cursor_.current_byte_ = '\0';
        } else {
            cursor_.current_byte_ = input_[cursor_.peek_pos_];
        }

        if (cursor_.current_byte_ == '\n') {
            cursor_.line_no_ += 1;
            cursor_.col_no_ = 0;
        } else {
            cursor_.col_no_ += 1;
        }

        cursor_.pos_ = cursor_.peek_pos_;
        cursor_.peek_pos_ += 1;
    }
}

auto lexer_t::read_operator() const noexcept -> stdx::option<token_t> {
    const auto start_line{cursor_.line_no_};
    const auto start_col{cursor_.col_no_};

    if (cursor_.current_byte_ == '\0') {
        return token_t{token_type_t::END_OF_FILE, {}, start_line, start_col};
    }

    usize max_len{0};
    auto  matched_type{token_type_t::ILLEGAL};

    // Try extending from length 1 up to the max operator size
    for (usize len{1}; len <= max_operator_length() && cursor_.pos_ + len <= input_.size(); ++len) {
        if (const auto op{get_operator_opt(stdx::string::substr(input_, cursor_.pos_, len))}) {
            matched_type = *op;
            max_len      = len;
        }
    }

    // We cannot greedily consume the lexer here since the next token instruction handles that
    if (max_len == 0) { return stdx::none; }
    return token_t{
        matched_type, stdx::string::substr(input_, cursor_.pos_, max_len), start_line, start_col};
}

auto lexer_t::read_ident() noexcept -> std::string_view {
    const auto start{cursor_.pos_};

    auto passed_first{false};
    while (std::isalpha(cursor_.current_byte_) || cursor_.current_byte_ == '_' ||
           (passed_first && std::isdigit(cursor_.current_byte_))) {
        read_character();
        passed_first = true;
    }

    return stdx::string::substr(input_, start, cursor_.pos_ - start);
}

auto lexer_t::read_number() noexcept -> token_t {
    const auto start{cursor_.pos_};
    const auto start_line{cursor_.line_no_};
    const auto start_col{cursor_.col_no_};
    auto       passed_decimal{false};
    auto       passed_exponent{false};

    // Consume digits and handle dot/range rules
    auto last_was_digit{false};
    while (true) {
        const auto c{cursor_.current_byte_};

        // Exponent handling defaults to floats for simplicity
        if (!passed_exponent && (c == 'e' || c == 'E')) {
            auto p{cursor_.peek_pos_};
            if (p >= input_.size()) { break; }

            auto next{input_[p]};
            if (next == '+' || next == '-') {
                p += 1;
                if (p >= input_.size()) { break; }
                next = input_[p];
            }

            if (!std::isdigit(next)) { break; }

            passed_exponent = true;
            read_character();

            if (cursor_.current_byte_ == '+' || cursor_.current_byte_ == '-') { read_character(); }
            while (std::isdigit(cursor_.current_byte_)) { read_character(); }
            last_was_digit = true;
            continue;
        }

        // Fractional part
        if (c == '.') {
            if (cursor_.peek_pos_ < input_.size() && input_[cursor_.peek_pos_] == '.') { break; }
            if (passed_decimal) { break; }

            passed_decimal = true;
            last_was_digit = false;
            read_character();
            continue;
        }

        // Underscore can only be in between digits
        if (c == '_' && last_was_digit) {
            read_character();
            if (!std::isdigit(cursor_.current_byte_)) {
                return {token_type_t::ILLEGAL,
                        stdx::string::substr(input_, start, cursor_.pos_ - start),
                        start_line,
                        start_col};
            }
            last_was_digit = false;
            continue;
        }

        // Normal digit
        if (std::isdigit(c)) {
            last_was_digit = true;
            read_character();
            continue;
        }

        break;
    }

    // Total validation
    const auto length{cursor_.pos_ - start};
    auto       type{token_type_t::ILLEGAL};
    if (length == 0) {
        return {type, stdx::string::substr(input_, start, 1), start_line, start_col};
    }

    if (input_[cursor_.pos_ - 1] == '.') {
        return {type, stdx::string::substr(input_, start, length), start_line, start_col};
    }

    // Determine the input type
    type = passed_decimal || passed_exponent ? token_type_t::FLOAT_LITERAL
                                             : token_type_t::INTEGER_LITERAL;
    return {type, stdx::string::substr(input_, start, length), start_line, start_col};
}

auto lexer_t::read_escape() noexcept -> char {
    read_character();

    switch (cursor_.current_byte_) {
    case 'n':  return '\n';
    case 'r':  return '\r';
    case 't':  return '\t';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"':  return '"';
    case '0':  return '\0';
    default:   return cursor_.current_byte_;
    }
}

auto lexer_t::read_string() noexcept -> token_t {
    const auto start{cursor_.pos_};
    const auto start_line{cursor_.line_no_};
    const auto start_col{cursor_.col_no_};
    read_character();

    while (cursor_.current_byte_ != '"' && cursor_.current_byte_ != '\0') {
        if (cursor_.current_byte_ == '\\') { read_escape(); }
        read_character();
    }

    if (cursor_.current_byte_ == '\0') {
        return {
            token_type_t::ILLEGAL,
            stdx::string::substr(input_, start, cursor_.pos_ - start),
            start_line,
            start_col,
        };
    }
    read_character();

    return {token_type_t::STRING_LITERAL,
            stdx::string::substr(input_, start, cursor_.pos_ - start),
            start_line,
            start_col};
}

// Reads a comment from the token, assuming the '//' operator has been consumed
auto lexer_t::read_comment() noexcept -> token_t {
    const auto start{cursor_.pos_};
    const auto start_line{cursor_.line_no_};
    const auto start_col{cursor_.col_no_};
    while (cursor_.current_byte_ != '\n' && cursor_.current_byte_ != '\0') { read_character(); }

    return {token_type_t::COMMENT,
            stdx::string::substr(input_, start, cursor_.pos_ - start),
            start_line,
            start_col};
}

} // namespace cairn::sql::syntax
