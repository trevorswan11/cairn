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
#include "storage/page.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace stdx::size_literals;

TEST_CASE("sql::catalog bootstrapping and metadata persistence") {
    helpers::tempfile db_file{"catalog_test_db"};
    helpers::tempfile log_file{"catalog_test_log"};

    using pool_t = storage::buffer_pool<64>;
    wal::log::manager lm{log_file.path, 1_MiB};

    storage::page_id_t saved_tables_root{storage::INVALID_PAGE_ID};
    storage::page_id_t saved_columns_root{storage::INVALID_PAGE_ID};
    storage::page_id_t saved_indexes_root{storage::INVALID_PAGE_ID};

    // Initial bootstrap of a clean database
    {
        auto pool{UNWRAP(pool_t::open(db_file.path))};
        pool->set_log_manager(lm);
        txn::undo::manager<i64, 64> undo_mgr{*pool};
        txn::manager                tm;
        sql::catalog<64>            cat{*pool, tm, undo_mgr, lm};
        REQUIRE(cat.bootstrap());

        saved_tables_root = cat.sys_tables_root();
        CHECK(saved_tables_root != storage::INVALID_PAGE_ID);
        saved_columns_root = cat.sys_columns_root();
        CHECK(saved_columns_root != storage::INVALID_PAGE_ID);
        saved_indexes_root = cat.sys_indexes_root();
        CHECK(saved_indexes_root != storage::INVALID_PAGE_ID);

        // Verify system tables exist
        auto sys_tables{UNWRAP(cat.get_table(sql::metadata::sys_tables_name))};
        CHECK(sys_tables.name == sql::metadata::sys_tables_name);
        CHECK(sys_tables.table_schema.column_count() == 3);

        auto sys_columns{UNWRAP(cat.get_table(sql::metadata::sys_columns_name))};
        CHECK(sys_columns.name == sql::metadata::sys_columns_name);
        CHECK(sys_columns.table_schema.column_count() == 4);

        auto sys_indexes{UNWRAP(cat.get_table(sql::metadata::sys_indexes_name))};
        CHECK(sys_indexes.name == sql::metadata::sys_indexes_name);
        CHECK(sys_indexes.table_schema.column_count() == 5);
    }

    // Restart and ensure bootstrapping is idempotent and reloads catalog
    {
        auto pool{UNWRAP(pool_t::open(db_file.path))};
        pool->set_log_manager(lm);
        txn::undo::manager<i64, 64> undo_mgr{*pool};
        txn::manager                tm;
        tm.set_next_txn_id(txn::id_t{10});
        sql::catalog<64> cat{*pool, tm, undo_mgr, lm};
        REQUIRE(cat.bootstrap());

        CHECK(cat.sys_tables_root() == saved_tables_root);
        CHECK(cat.sys_columns_root() == saved_columns_root);
        CHECK(cat.sys_indexes_root() == saved_indexes_root);

        auto sys_tables{cat.get_table(sql::metadata::sys_tables_name)};
        REQUIRE(sys_tables.has_value());
        CHECK(sys_tables->name == sql::metadata::sys_tables_name);
    }
}

TEST_CASE("sql::catalog restart persistence of user metadata") {
    helpers::tempfile db_file{"catalog_persist_test_db"};
    helpers::tempfile log_file{"catalog_persist_test_log"};

    using pool_t = storage::buffer_pool<64>;
    wal::log::manager lm{log_file.path, 1_MiB};

    // Create table and close database
    {
        auto pool{UNWRAP(pool_t::open(db_file.path))};
        pool->set_log_manager(lm);
        txn::undo::manager<i64, 64> undo_mgr{*pool};
        txn::manager                tm;
        sql::catalog<64>            cat{*pool, tm, undo_mgr, lm};
        REQUIRE(cat.bootstrap());

        const auto  transaction{tm.begin_txn()};
        sql::schema sch{{sql::column{"key", sql::type::id_t::BIGINT, false},
                         sql::column{"value", sql::type::id_t::VARCHAR, true}}};
        REQUIRE(cat.create_table(transaction, sql::table_id_t{42}, "items", sch));
        REQUIRE(cat.create_index(
            transaction, sql::table_id_t{42}, sql::index_id_t{99}, "idx_items_key", 0, false));

        REQUIRE(tm.update_txn_lsn(transaction, wal::log::seq_num{2}));
        REQUIRE(tm.commit_txn(transaction, lm));
    }

    // Reopen and verify metadata persists
    {
        auto pool{UNWRAP(pool_t::open(db_file.path))};
        pool->set_log_manager(lm);
        txn::undo::manager<i64, 64> undo_mgr{*pool};
        txn::manager                tm;
        tm.set_next_txn_id(txn::id_t{10});
        sql::catalog<64> cat{*pool, tm, undo_mgr, lm};
        REQUIRE(cat.bootstrap());

        // Verify table persisted
        auto table{UNWRAP(cat.get_table("items"))};
        CHECK(table.table_id == sql::table_id_t{42});
        CHECK(table.table_schema.column_count() == 2);
        CHECK(table.table_schema[0].name() == "key");
        CHECK(table.table_schema[0].type() == sql::type::id_t::BIGINT);
        CHECK(table.table_schema[1].name() == "value");
        CHECK(table.table_schema[1].type() == sql::type::id_t::VARCHAR);

        // Verify index persisted
        auto idx{UNWRAP(cat.get_index("idx_items_key"))};
        CHECK(idx.table_id == sql::table_id_t{42});
        CHECK(idx.index_id == sql::index_id_t{99});
        CHECK(idx.column_id == 0);
        CHECK(idx.is_unique == false);
    }
}

} // namespace cairn::tests
