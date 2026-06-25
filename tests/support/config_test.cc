#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>

#include "support/config.hh"
#include "support/error.hh"
#include "testhelpers/common.hh"

namespace cairn::tests {

TEST_CASE("Well-formed config parsing") {
    auto logging = GENERATE(true, false);

    const auto input{fmt::format(
        R"({{
        "development": {{
            "host": "127.0.0.1",
            "port": 5432,
            "database": "my_app_dev",
            "username": "db_user",
            "password": "secretpassword"
            {}, "logfile": "log.log"
          }}
        }})",
        logging ? "" : "//")};
    const auto config{helpers::unwrap(support::config::parse("development", input))};

    CHECK(config.host == "127.0.0.1");
    CHECK(config.port == 5'432);
    CHECK(config.database == "my_app_dev");
    CHECK(config.username == "db_user");
    CHECK(config.password == "secretpassword");

    if (logging) {
        CHECK(config.logfile == "log.log");
    } else {
        CHECK_FALSE(config.logfile);
    }
}

TEST_CASE("Config not nested inside of object") {
    const auto err = helpers::unwrap_err(support::config::parse("development", R"("development": {
            "host": "127.0.0.1",
            "port": 5432,
            "database": "my_app_dev",
            "username": "db_user",
            "password": "secretpassword",
            "logfile": "log.log"
    })"));
    CHECK(err == support::error_t::JSON_PARSE_ERROR);
}

TEST_CASE("Config with missing required fields") {
    const auto err = helpers::unwrap_err(support::config::parse("development", R"({
        "development": {}
    })"));
    CHECK(err == support::error_t::JSON_MISSING_FIELD);
}

TEST_CASE("Config with mistyped field value") {
    const auto err = helpers::unwrap_err(support::config::parse("development", R"({
        "development": {
            "host": "127.0.0.1",
            "port": "5432",
            "database": "my_app_dev",
            "username": "db_user",
            "password": "secretpassword",
            "logfile": "log.log"
          }
    })"));
    CHECK(err == support::error_t::JSON_TYPE_ERROR);
}

} // namespace cairn::tests
