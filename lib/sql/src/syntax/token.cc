#include "sql/syntax/token.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <string_view>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <stdx/enum.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::sql::syntax {

namespace {

using namespace std::string_view_literals;

constexpr auto ALL_OPERATORS{[] {
    constexpr std::array operators{std::pair{"+"sv, token_type_t::PLUS},
                                   std::pair{"-"sv, token_type_t::MINUS},
                                   std::pair{"*"sv, token_type_t::STAR},
                                   std::pair{"/"sv, token_type_t::SLASH},
                                   std::pair{"="sv, token_type_t::EQ},
                                   std::pair{"!="sv, token_type_t::NEQ},
                                   std::pair{"<"sv, token_type_t::LT},
                                   std::pair{"<="sv, token_type_t::LT_EQ},
                                   std::pair{">"sv, token_type_t::GT},
                                   std::pair{">="sv, token_type_t::GT_EQ},
                                   std::pair{","sv, token_type_t::COMMA},
                                   std::pair{"."sv, token_type_t::DOT},
                                   std::pair{"#"sv, token_type_t::COMMENT},
                                   std::pair{"--"sv, token_type_t::COMMENT}};

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

constexpr auto ALL_KEYWORDS{[] {
    constexpr auto types{stdx::enum_range<token_type_t::SELECT, token_type_t::KW_OR>()};
    stdx::fixed::
        hash_map<std::string_view, token_type_t, types.size(), stdx::crc::hash, keyword_equals>
            map;
    for (const auto& type : types) { map.emplace(magic_enum::enum_name(type), type); }
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
    return ALL_OPERATORS.get_opt(sv).materialize();
}

auto get_keyword_opt(std::string_view sv) noexcept -> stdx::option<token_type_t> {
    return ALL_KEYWORDS.get_opt(sv).materialize();
}

} // namespace cairn::sql::syntax
