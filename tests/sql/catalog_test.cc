#include <cstddef>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "sql/catalog.hh"
#include "sql/schema.hh"
#include "sql/type.hh"
#include "storage/buffer_pool.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::sql;
using namespace stdx::size_literals;

TEST_CASE("sql::catalog CRUD operations for tables and indexes") {
    helpers::tempfile db_file{"catalog_crud_test_db"};
    helpers::tempfile log_file{"catalog_crud_test_log"};

    using pool_t = storage::buffer_pool<64>;
    wal::log::manager lm{log_file.path, 1_MiB};
    auto              pool{UNWRAP(pool_t::open(db_file.path))};
    pool->set_log_manager(lm);

    txn::undo::manager<i64, 64> undo_mgr{*pool};
    txn::manager                tm;
    catalog<64>                 cat{*pool, tm, undo_mgr, lm};
    REQUIRE(cat.bootstrap());

    // Create table "users"
    {
        const auto transaction{tm.begin_txn()};
        schema     user_schema{{column{"id", type::id_t::INTEGER, false},
                                column{"name", type::id_t::VARCHAR, false},
                                column{"active", type::id_t::BOOLEAN, true}}};

        auto meta{UNWRAP(cat.create_table(transaction, table_id_t{10}, "users", user_schema))};
        CHECK(meta.table_id == table_id_t{10});
        CHECK(meta.name == "users");
        CHECK(meta.table_schema.column_count() == 3);
        REQUIRE(tm.update_txn_lsn(transaction, wal::log::seq_num{2}));
        REQUIRE(tm.commit_txn(transaction, lm));
    }

    // Verify user table lookups in cache
    auto users_table{UNWRAP(cat.get_table("users"))};
    CHECK(users_table.table_id == table_id_t{10});
    CHECK(users_table.table_schema[1].name() == "name");
    CHECK(users_table.table_schema[2].type() == type::id_t::BOOLEAN);
    CHECK(UNWRAP(cat.get_table(table_id_t{10})).name == "users");

    // Create index "idx_users_id"
    {
        const auto transaction{tm.begin_txn()};
        auto       idx_meta{UNWRAP(cat.create_index(
            transaction, table_id_t{10}, index_id_t{100}, "idx_users_id", 0, true))};
        CHECK(idx_meta.table_id == table_id_t{10});
        CHECK(idx_meta.index_id == index_id_t{100});
        CHECK(idx_meta.name == "idx_users_id");
        CHECK(idx_meta.column_id == 0);
        CHECK(idx_meta.is_unique == true);
        REQUIRE(tm.update_txn_lsn(transaction, wal::log::seq_num{3}));
        REQUIRE(tm.commit_txn(transaction, lm));
    }

    // Lookup index in cache
    CHECK(UNWRAP(cat.get_index("idx_users_id")).index_id == index_id_t{100});
    CHECK(UNWRAP(cat.get_index(index_id_t{100})).name == "idx_users_id");

    // Drop index
    {
        const auto transaction{tm.begin_txn()};
        REQUIRE(cat.drop_index(transaction, "idx_users_id"));
        REQUIRE(tm.update_txn_lsn(transaction, wal::log::seq_num{4}));
        REQUIRE(tm.commit_txn(transaction, lm));

        CHECK_FALSE(cat.get_index("idx_users_id").has_value());
        CHECK_FALSE(cat.get_index(index_id_t{100}).has_value());
    }

    // Drop table
    {
        const auto transaction{tm.begin_txn()};
        REQUIRE(cat.drop_table(transaction, "users"));
        REQUIRE(tm.update_txn_lsn(transaction, wal::log::seq_num{5}));
        REQUIRE(tm.commit_txn(transaction, lm));

        CHECK_FALSE(cat.get_table("users").has_value());
        CHECK_FALSE(cat.get_table(table_id_t{10}).has_value());
    }
}

} // namespace cairn::tests
