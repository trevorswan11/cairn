#include "sql/syntax/token.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <string_view>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <stdx/assert.hh>
#include <stdx/enum.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

namespace cairn::sql::syntax {

auto misc_type_from_char(char c) noexcept -> stdx::option<token_type_t> {
    switch (c) {
    case ',': return token_type_t::COMMA;
    case ';': return token_type_t::SEMICOLON;
    case '(': return token_type_t::LPAREN;
    case ')': return token_type_t::RPAREN;
    default:  return stdx::none;
    }
}

namespace {

constexpr auto ALL_OPERATORS{[] {
    constexpr std::array operators{std::pair{"+", token_type_t::PLUS},
                                   std::pair{"-", token_type_t::MINUS},
                                   std::pair{"*", token_type_t::STAR},
                                   std::pair{"/", token_type_t::SLASH},
                                   std::pair{"=", token_type_t::EQ},
                                   std::pair{"!=", token_type_t::NEQ},
                                   std::pair{"<", token_type_t::LT},
                                   std::pair{"<=", token_type_t::LT_EQ},
                                   std::pair{">", token_type_t::GT},
                                   std::pair{">=", token_type_t::GT_EQ},
                                   std::pair{",", token_type_t::COMMA},
                                   std::pair{".", token_type_t::DOT},
                                   std::pair{"#", token_type_t::COMMENT},
                                   std::pair{"--", token_type_t::COMMENT}};

    stdx::fixed::hash_map<std::string_view, token_type_t, operators.size(), stdx::crc::hash> map;
    for (const auto& op : operators) { map.emplace(op.first, op.second); }
    return map;
}()};

// Case insensitive equality for SQL keyword comparison
struct keyword_equals {
    [[nodiscard]] static constexpr auto operator()(std::string_view lhs,
                                                   std::string_view rhs) noexcept -> bool {
        if (lhs.size() != rhs.size()) { return false; }
        for (const auto [l, r] : std::views::zip(lhs, rhs)) {
            if (std::tolower(l) != std::tolower(r)) { return false; }
        }
        return true;
    }
};

constexpr auto ALL_KEYWORD_TYPES{stdx::enum_range<token_type_t::SELECT, token_type_t::KW_OR>()};

constexpr auto ALL_KEYWORDS{[] {
    stdx::fixed::hash_map<std::string_view,
                          token_type_t,
                          ALL_KEYWORD_TYPES.size(),
                          stdx::crc::hash,
                          keyword_equals>
        map;
    for (const auto& type : ALL_KEYWORD_TYPES) { map.emplace(magic_enum::enum_name(type), type); }
    return map;
}()};

} // namespace

auto max_operator_length() noexcept -> usize {
    return std::ranges::max_element(
               ALL_OPERATORS,
               [](auto a, auto b) -> bool { return a.first.size() < b.first.size(); })
        ->first.size();
}

auto get_operator_opt(std::string_view sv) noexcept -> stdx::option<token_type_t> {
    PROFILE_FUNCTION();
    return ALL_OPERATORS.get_opt(sv).materialize();
}

auto get_keyword_opt(std::string_view sv) noexcept -> stdx::option<token_type_t> {
    PROFILE_FUNCTION();
    return ALL_KEYWORDS.get_opt(sv).materialize();
}

auto token_t::is_valid_ident() const noexcept -> bool {
    PROFILE_FUNCTION();
    return type == token_type_t::IDENTIFIER || std::ranges::binary_search(ALL_KEYWORD_TYPES, type);
}

} // namespace cairn::sql::syntax
