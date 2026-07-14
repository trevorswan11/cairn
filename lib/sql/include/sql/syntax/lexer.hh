#pragma once

#include <iterator>
#include <string_view>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "sql/syntax/token.hh"

namespace cairn::sql::syntax {

class lexer_t {
  public:
    class iterator_t {
      public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = token_t;
        using difference_type   = idiff;
        using pointer           = const token_t*;
        using reference         = const token_t&;

      public:
        constexpr iterator_t(lexer_t& lexer, const token_t& current_token)
            : lexer_{lexer}, current_token_{current_token} {}

        constexpr auto operator++() -> iterator_t& {
            current_token_ = lexer_.advance();
            return *this;
        }

        constexpr auto operator*() const noexcept -> reference { return current_token_; }
        constexpr auto operator->() const noexcept -> pointer { return &current_token_; }

        [[nodiscard]] constexpr auto operator==(std::default_sentinel_t) const noexcept -> bool {
            return current_token_.type == token_type_t::END_OF_FILE;
        }

      private:
        lexer_t& lexer_;
        token_t  current_token_;
    };

    class cursor_t {
        usize pos_{0};
        usize peek_pos_{0};
        char  current_char_{0};
        usize line_no_{0};
        usize col_no_{0};

        // Only the lexer can access state to protect invariants
        friend class lexer_t;
    };

  public:
    lexer_t() noexcept = default;
    explicit lexer_t(std::string_view input) noexcept;

    auto               reset(std::string_view input = {}) noexcept -> void;
    auto               advance() noexcept -> token_t;
    [[nodiscard]] auto consume() -> std::vector<token_t>;

    auto        begin() noexcept -> iterator_t { return iterator_t{*this, advance()}; }
    static auto end() noexcept -> std::default_sentinel_t { return std::default_sentinel; }

    constexpr auto restore(const cursor_t& cursor) noexcept -> void { cursor_ = cursor; }
    [[nodiscard]] constexpr auto snapshot() const noexcept -> cursor_t { return cursor_; }

  private:
    auto skip_whitespace() noexcept -> void;

    // Reads n characters from the input stream
    auto               step_cursor(u8 times = 1) noexcept -> void;
    [[nodiscard]] auto read_operator() const noexcept -> stdx::option<token_t>;
    auto               read_ident() noexcept -> std::string_view;
    auto               read_number() noexcept -> token_t;
    auto               read_string() noexcept -> token_t;
    auto               read_escape() noexcept -> char;
    auto               read_comment(const token_t& comment_op) noexcept -> token_t;

  private:
    std::string_view input_;
    cursor_t         cursor_;
};

} // namespace cairn::sql::syntax
