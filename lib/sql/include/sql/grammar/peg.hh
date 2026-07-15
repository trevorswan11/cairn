#pragma once

#include <tao/pegtl.hpp>
#include <tao/pegtl/ascii.hpp>
#include <tao/pegtl/pegtl_string.hpp>
#include <tao/pegtl/rules.hpp>

namespace cairn::sql::grammar {

namespace peg = tao::pegtl;

struct line_comment : peg::seq<peg::string<'-', '-'>, peg::until<peg::eolf>> {};
struct hash_comment : peg::seq<peg::one<'#'>, peg::until<peg::eolf>> {};
struct block_comment : peg::seq<peg::string<'/', '*'>, peg::until<peg::string<'*', '/'>>> {};

struct sep : peg::sor<peg::ascii::space, line_comment, hash_comment, block_comment> {};
struct opt_space : peg::star<sep> {};
struct mand_space : peg::plus<sep> {};

template <typename Rule> struct padded : peg::pad<Rule, sep> {};

#define SQL_KEYWORD(name, str) \
    struct name : TAO_PEGTL_ISTRING(str) {}

SQL_KEYWORD(select_kw, "SELECT");
SQL_KEYWORD(from_kw, "FROM");
SQL_KEYWORD(where_kw, "WHERE");
SQL_KEYWORD(create_kw, "CREATE");
SQL_KEYWORD(drop_kw, "DROP");
SQL_KEYWORD(alter_kw, "ALTER");
SQL_KEYWORD(table_kw, "TABLE");
SQL_KEYWORD(index_kw, "INDEX");
SQL_KEYWORD(add_kw, "ADD");
SQL_KEYWORD(column_kw, "COLUMN");
SQL_KEYWORD(on_kw, "ON");
SQL_KEYWORD(null_kw, "NULL");
SQL_KEYWORD(not_kw, "NOT");
SQL_KEYWORD(and_kw, "AND");
SQL_KEYWORD(or_kw, "OR");
SQL_KEYWORD(true_kw, "TRUE");
SQL_KEYWORD(false_kw, "FALSE");

SQL_KEYWORD(boolean_kw, "BOOLEAN");
SQL_KEYWORD(tinyint_kw, "TINYINT");
SQL_KEYWORD(smallint_kw, "SMALLINT");
SQL_KEYWORD(int_kw, "INT");
SQL_KEYWORD(integer_kw, "INTEGER");
SQL_KEYWORD(bigint_kw, "BIGINT");
SQL_KEYWORD(float_kw, "FLOAT");
SQL_KEYWORD(double_kw, "DOUBLE");
SQL_KEYWORD(varchar_kw, "VARCHAR");
SQL_KEYWORD(datetime_kw, "DATETIME");

#undef SQL_KEYWORD

struct string_literal
    : peg::seq<peg::one<'\''>,
               peg::star<peg::sor<peg::string<'\\', '\''>, peg::not_one<'\'', '\0'>>>,
               peg::one<'\''>> {};

struct decimal_digits : peg::plus<peg::ascii::digit> {};
struct float_exponent : peg::seq<peg::one<'e', 'E'>, peg::opt<peg::one<'+', '-'>>, decimal_digits> {
};

struct numeric_literal
    : peg::seq<peg::opt<peg::one<'+', '-'>>,
               peg::sor<peg::seq<decimal_digits, peg::one<'.'>, peg::star<peg::ascii::digit>>,
                        peg::seq<peg::one<'.'>, decimal_digits>,
                        decimal_digits>,
               peg::opt<float_exponent>> {};

struct unquoted_ident : peg::ascii::identifier {};
struct backticked_ident : peg::seq<peg::one<'`'>, peg::until<peg::one<'`'>>> {};
struct double_quoted_ident : peg::seq<peg::one<'"'>, peg::until<peg::one<'"'>>> {};
struct identifier : peg::sor<unquoted_ident, backticked_ident, double_quoted_ident> {};

struct expression;

struct primary_expr : peg::sor<boolean_kw,
                               null_kw,
                               string_literal,
                               numeric_literal,
                               identifier,
                               peg::seq<peg::one<'('>, padded<expression>, peg::one<')'>>> {};

struct op_plus : peg::one<'+'> {};
struct op_minus : peg::one<'-'> {};
struct op_mul : peg::one<'*'> {};
struct op_div : peg::one<'/'> {};
struct op_eq : peg::one<'='> {};
struct op_neq : peg::sor<peg::string<'!', '='>, peg::string<'<', '>'>> {};
struct op_lte : peg::string<'<', '='> {};
struct op_gte : peg::string<'>', '='> {};
struct op_lt : peg::one<'<'> {};
struct op_gt : peg::one<'>'> {};
struct binary_op : peg::sor<op_neq,
                            op_lte,
                            op_gte,
                            op_eq,
                            op_lt,
                            op_gt,
                            op_plus,
                            op_minus,
                            op_mul,
                            op_div,
                            and_kw,
                            or_kw> {};

struct expression : peg::list<primary_expr, padded<binary_op>> {};

struct select_all : peg::one<'*'> {};
struct select_item : peg::sor<select_all, expression> {};
struct select_list : peg::list<select_item, padded<peg::one<','>>> {};

struct select_stmt : peg::seq<select_kw,
                              mand_space,
                              select_list,
                              mand_space,
                              from_kw,
                              mand_space,
                              identifier,
                              peg::opt<peg::seq<mand_space, where_kw, mand_space, expression>>> {};

struct data_type : peg::sor<boolean_kw,
                            tinyint_kw,
                            smallint_kw,
                            integer_kw,
                            int_kw,
                            bigint_kw,
                            float_kw,
                            double_kw,
                            varchar_kw,
                            datetime_kw> {};

struct column_def : peg::seq<identifier,
                             mand_space,
                             data_type,
                             peg::opt<peg::seq<mand_space, not_kw, mand_space, null_kw>>,
                             peg::opt<peg::seq<mand_space, null_kw>>> {};

struct column_defs : peg::list<column_def, padded<peg::one<','>>> {};
struct create_table_stmt : peg::seq<create_kw,
                                    mand_space,
                                    table_kw,
                                    mand_space,
                                    identifier,
                                    padded<peg::one<'('>>,
                                    column_defs,
                                    padded<peg::one<')'>>> {};

struct drop_table_stmt : peg::seq<drop_kw, mand_space, table_kw, mand_space, identifier> {};
struct drop_index_stmt : peg::seq<drop_kw,
                                  mand_space,
                                  index_kw,
                                  mand_space,
                                  identifier,
                                  mand_space,
                                  on_kw,
                                  mand_space,
                                  identifier> {};

struct alter_table_stmt : peg::seq<alter_kw,
                                   mand_space,
                                   table_kw,
                                   mand_space,
                                   identifier,
                                   mand_space,
                                   add_kw,
                                   mand_space,
                                   peg::opt<peg::seq<column_kw, mand_space>>,
                                   column_def> {};

struct index_columns : peg::list<identifier, padded<peg::one<','>>> {};
struct create_index_stmt : peg::seq<create_kw,
                                    mand_space,
                                    index_kw,
                                    mand_space,
                                    identifier,
                                    mand_space,
                                    on_kw,
                                    mand_space,
                                    identifier,
                                    padded<peg::one<'('>>,
                                    index_columns,
                                    padded<peg::one<')'>>> {};

struct statement : peg::sor<select_stmt,
                            create_table_stmt,
                            drop_table_stmt,
                            alter_table_stmt,
                            create_index_stmt,
                            drop_index_stmt> {};
struct sql_grammar
    : peg::seq<opt_space, statement, opt_space, peg::opt<peg::one<';'>>, opt_space, peg::eof> {};

} // namespace cairn::sql::grammar
