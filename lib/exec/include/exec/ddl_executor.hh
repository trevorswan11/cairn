#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

#include <gsl/span>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "exec/table_scan.hh"
#include "sql/binder/nodes.hh"
#include "sql/catalog.hh"
#include "sql/schema.hh"
#include "sql/tuple.hh"
#include "sql/type.hh"
#include "sql/value.hh"
#include "storage/bplus.hh"
#include "storage/buffer_pool.hh"
#include "support/diagnostic/error.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"

namespace cairn::exec {

template <usize PoolSize> class ddl_executor {
  public:
    explicit ddl_executor(sql::catalog<PoolSize>&         catalog,
                          storage::buffer_pool<PoolSize>& pool,
                          txn::manager&                   txn_mgr) noexcept
        : catalog_{catalog}, pool_{pool}, txn_mgr_{txn_mgr} {}

    [[nodiscard]] auto execute_create_table(txn::id_t                               txn_id,
                                            sql::table_id_t                         table_id,
                                            const sql::binder::create_table_stmt_t& stmt)
        -> result<void> {
        std::vector<sql::column> columns;
        columns.reserve(stmt.column_defs.size());
        for (const auto& col_def : stmt.column_defs) {
            columns.emplace_back(
                col_def.name, col_def.type.value_or(sql::type::id_t::INTEGER), col_def.nullable);
        }
        sql::schema sch{std::move(columns)};

        TRY(catalog_.create_table(txn_id, table_id, stmt.table_name.view(), sch));
        return {};
    }

    [[nodiscard]] auto execute_drop_table(txn::id_t                             txn_id,
                                          const sql::binder::drop_table_stmt_t& stmt)
        -> result<void> {
        // Drop all indexes associated with the table then the table itself
        auto indexes{catalog_.get_table_indexes(stmt.table_id)};
        for (const auto& idx : indexes) { TRY(catalog_.drop_index(txn_id, idx.name.view())); }
        return catalog_.drop_table(txn_id, stmt.table_name.view());
    }

    [[nodiscard]] auto execute_alter_table(txn::id_t                              txn_id,
                                           const sql::binder::alter_table_stmt_t& stmt)
        -> result<void> {
        auto tbl_opt{catalog_.get_table(stmt.table_id)};
        if (!tbl_opt) { return stdx::err{error::SQL_TABLE_NOT_FOUND}; }
        const auto& tbl{*tbl_opt};

        // Read all rows from the existing table B+ tree
        using txn_tree_t = txn::iot_tree<i64, 128, PoolSize>;
        typename txn_tree_t::tree_t tree_impl{pool_, tbl.root_page_id};
        txn_tree_t                  primary_tree{tree_impl, catalog_.undo_manager()};

        std::vector<std::pair<i64, std::vector<sql::value_t>>> rows;
        const i64 low{0}, high{std::numeric_limits<i64>::max()};

        const auto                     snap{TRY(txn_mgr_.acquire_snapshot(txn_id))};
        table_scan<i64, 128, PoolSize> scanner{primary_tree, txn_id, snap, txn_mgr_};

        TRY(scanner(low, high, [&](const i64& key, gsl::span<const std::byte> val) {
            sql::tuple::byte_buffer buf;
            buf.resize(val.size());
            std::memcpy(buf.data(), val.data(), val.size());
            sql::tuple t{std::move(buf)};

            std::vector<sql::value_t> vals(tbl.table_schema.column_count());
            if (t.deserialize(tbl.table_schema, vals)) { rows.emplace_back(key, std::move(vals)); }
        }));

        // Perform the schema upgrade
        std::vector<sql::column> new_columns;
        new_columns.reserve(tbl.table_schema.column_count());
        for (const auto& col : tbl.table_schema.columns()) { new_columns.emplace_back(col); }
        const bool is_add{stmt.column_def.type.has_value()};

        if (is_add) {
            new_columns.emplace_back(
                stmt.column_def.name, *stmt.column_def.type, stmt.column_def.nullable);
        } else {
            // DROP COLUMN
            auto it{std::find_if(new_columns.begin(), new_columns.end(), [&](const auto& col) {
                return col.name() == stmt.column_def.name.view();
            })};
            if (it == new_columns.end()) { return stdx::err{error::SQL_COLUMN_NOT_FOUND}; }
            const auto drop_idx{std::distance(new_columns.begin(), it)};
            new_columns.erase(it);

            // Drop any indexes on the dropped column, and shift other indexes
            auto tbl_indexes{catalog_.get_table_indexes(tbl.table_id)};
            for (const auto& idx : tbl_indexes) {
                if (idx.column_id == static_cast<i32>(drop_idx)) {
                    TRY(catalog_.drop_index(txn_id, idx.name.view()));
                } else if (idx.column_id > static_cast<i32>(drop_idx)) {
                    TRY(catalog_.update_index_column_id(txn_id, idx.index_id, idx.column_id - 1));
                }
            }
        }

        // Rewrite all rows using the new schema
        sql::schema new_schema{std::move(new_columns)};
        for (const auto& [key, old_vals] : rows) {
            std::vector<sql::value_t> new_vals;
            if (is_add) {
                new_vals = old_vals;
                new_vals.emplace_back(sql::value_t::make_null(*stmt.column_def.type));
            } else {
                usize drop_idx{0};
                for (usize i{0}; i < tbl.table_schema.column_count(); ++i) {
                    if (tbl.table_schema[i].name() == stmt.column_def.name.view()) {
                        drop_idx = i;
                        break;
                    }
                }
                for (usize i{0}; i < old_vals.size(); ++i) {
                    if (i != drop_idx) { new_vals.emplace_back(old_vals[i]); }
                }
            }

            auto new_tuple{TRY(sql::tuple::serialize(new_schema, new_vals))};
            TRY(primary_tree.update_txn(txn_id, key, new_tuple.data()));
        }

        // Update the catalog metadata
        return catalog_.alter_table_schema(txn_id, tbl.table_id, new_schema);
    }

    [[nodiscard]] auto execute_create_index(txn::id_t                               txn_id,
                                            sql::index_id_t                         index_id,
                                            const sql::binder::create_index_stmt_t& stmt)
        -> result<void> {
        auto tbl_opt{catalog_.get_table(stmt.table_id)};
        if (!tbl_opt) { return stdx::err{error::SQL_TABLE_NOT_FOUND}; }
        const auto& tbl{*tbl_opt};

        if (stmt.column_indices.empty()) { return stdx::err{error::SQL_COLUMN_NOT_FOUND}; }
        const auto column_id{static_cast<i32>(stmt.column_indices[0])};

        // Create the index metadata and tree
        const auto idx_meta{TRY(catalog_.create_index(
            txn_id, stmt.table_id, index_id, stmt.index_name, column_id, false))};

        // Build B+ tree index over existing table data
        using txn_tree_t = txn::iot_tree<i64, 128, PoolSize>;
        typename txn_tree_t::tree_t tree_impl{pool_, tbl.root_page_id};
        txn_tree_t                  primary_tree{tree_impl, catalog_.undo_manager()};

        using index_tree_t = storage::bplus_tree<i64, i64, PoolSize>;
        index_tree_t secondary_index{pool_, idx_meta.root_page_id};
        const i64    low{0}, high{std::numeric_limits<i64>::max()};

        const auto                     snap{TRY(txn_mgr_.acquire_snapshot(txn_id))};
        table_scan<i64, 128, PoolSize> scanner{primary_tree, txn_id, snap, txn_mgr_};

        TRY(scanner(low, high, [&](const i64& key, gsl::span<const std::byte> val) {
            sql::tuple::byte_buffer buf;
            buf.resize(val.size());
            std::memcpy(buf.data(), val.data(), val.size());
            sql::tuple t{std::move(buf)};

            std::vector<sql::value_t> vals(tbl.table_schema.column_count());
            if (t.deserialize(tbl.table_schema, vals)) {
                auto col_val{vals[static_cast<usize>(column_id)]};
                if (!col_val.is_null()) {
                    DISCARD(secondary_index.emplace(
                        col_val.get_value().visit([&](bool v) -> i64 { return v ? 1 : 0; },
                                                  [&](i8 v) -> i64 { return v; },
                                                  [&](i16 v) -> i64 { return v; },
                                                  [&](i32 v) -> i64 { return v; },
                                                  [&](i64 v) -> i64 { return v; },
                                                  [&](f32 v) -> i64 { return static_cast<i64>(v); },
                                                  [&](f64 v) -> i64 { return static_cast<i64>(v); },
                                                  [&](auto) -> i64 { return 0; }),
                        key));
                }
            }
        }));
        return {};
    }

    [[nodiscard]] auto execute_drop_index(txn::id_t                             txn_id,
                                          const sql::binder::drop_index_stmt_t& stmt)
        -> result<void> {
        return catalog_.drop_index(txn_id, stmt.index_name.view());
    }

  private:
    sql::catalog<PoolSize>&         catalog_;
    storage::buffer_pool<PoolSize>& pool_;
    txn::manager&                   txn_mgr_;
};

} // namespace cairn::exec
