#include <string>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>
#include <stdx/utility.hh>

#include "sql/file.hh"
#include "sql/parser/parser.hh"

namespace cairn::tests::fuzz {

using namespace fuzztest;

auto FuzzSQLParser(const std::string& query) -> void {
    const sql::file source_file{query};
    DISCARD(sql::parser::parse(source_file));
}

FUZZ_TEST(SQLParserFuzz, FuzzSQLParser).WithDomains(Arbitrary<std::string>());

} // namespace cairn::tests::fuzz
