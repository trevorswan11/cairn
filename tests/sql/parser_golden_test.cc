#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/result.hh>

#include "sql/file.hh"
#include "sql/parser/dumper.hh"
#include "sql/parser/parser.hh"
#include "support/diagnostic/error.hh"
#include "testhelpers/env.hh"
#include "testhelpers/unwrap.hh"

namespace fs = std::filesystem;

namespace cairn::tests {

using namespace cairn::sql;

namespace {

[[nodiscard]] auto read_file_content(const fs::path& path) -> stdx::option<std::string> {
    if (std::ifstream ifs{path, std::ios::in | std::ios::binary}) {
        std::ostringstream oss;
        oss << ifs.rdbuf();
        return oss.str();
    }
    return stdx::none;
}

[[nodiscard]] auto write_file_content(const fs::path& path, const std::string& content)
    -> result<void> {
    fs::create_directories(path.parent_path());
    std::ofstream ofs{path, std::ios::out | std::ios::binary | std::ios::trunc};
    if (ofs) {
        ofs << content;
        return {};
    }
    return stdx::err{error::IO_ERROR};
}

} // namespace

TEST_CASE("Parser AST Golden Regression Tests") {
    const fs::path queries_dir{"tests/sql/queries"};
    const fs::path golden_dir{"tests/sql/golden"};

    REQUIRE(fs::exists(queries_dir));
    const auto update_golden{helpers::get_env("UPDATE_GOLDEN").value_or("") == "1"};

    for (const auto& entry : fs::directory_iterator(queries_dir)) {
        if (entry.path().extension() != ".sql") { continue; }

        const auto query_path{entry.path()};
        const auto filename{query_path.filename().replace_extension(".txt")};
        const auto golden_path{golden_dir / filename};

        const file source_file{UNWRAP(read_file_content(query_path))};
        const auto actual_dump{UNWRAP(
            parser::parse(source_file)
                .transform([](const auto& ast) {
                    std::ostringstream oss;
                    parser::dumper_t   dumper{ast, oss};
                    for (const auto node : ast) { dumper.dump(node); }
                    return oss.str();
                })
                .or_else([](const auto& loc) -> stdx::result<std::string, diagnostic> {
                    return fmt::format("Parse Error at line {}, column {}\n", loc.line, loc.column);
                }))};

        if (update_golden) {
            REQUIRE(write_file_content(golden_path, actual_dump));
        } else {
            REQUIRE(fs::exists(golden_path));
            CHECK(actual_dump == UNWRAP(read_file_content(golden_path)));
        }
    }
}

} // namespace cairn::tests
