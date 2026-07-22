#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/enum.hh>
#include <stdx/fixed/string.hh>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "exec/table_scan.hh"
#include "sql/schema.hh"
#include "sql/tuple.hh"
#include "sql/type.hh"
#include "sql/value.hh"
#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "support/diagnostic/error.hh"
#include "txn/id.hh"
#include "txn/iot_tree.hh"
#include "txn/manager.hh"
#include "txn/undo/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/seq_num.hh"

namespace cairn::sql {

enum class table_id_t : i64 {};
enum class index_id_t : i64 {};

namespace metadata {

struct table {
    table_id_t          table_id;
    stdx::fixed::string name;
    storage::page_id_t  root_page_id;
    schema              table_schema;
};

struct index {
    table_id_t          table_id;
    index_id_t          index_id;
    stdx::fixed::string name;
    i32                 column_id;
    storage::page_id_t  root_page_id;
    bool                is_unique;
};

constexpr std::array sys_header_magic{"CAIRNDB"};

struct db_header_t {
    std::remove_const_t<decltype(sys_header_magic)> magic;
    i64                                             sys_tables_root;
    i64                                             sys_columns_root;
    i64                                             sys_indexes_root;
};

[[nodiscard]] auto sys_tables_schema() -> const schema&;
[[nodiscard]] auto sys_columns_schema() -> const schema&;
[[nodiscard]] auto sys_indexes_schema() -> const schema&;

constexpr std::string_view sys_tables_name{"sys_tables"};
constexpr std::string_view sys_columns_name{"sys_columns"};
constexpr std::string_view sys_indexes_name{"sys_indexes"};

} // namespace metadata

template <usize PoolSize> class catalog {
  public:
    using pool_t         = storage::buffer_pool<PoolSize>;
    using txn_tree_t     = txn::iot_tree<i64, 128, PoolSize>;
    using catalog_scan_t = exec::table_scan<i64, 128, PoolSize>;

    using tables_by_name_map_t = ankerl::unordered_dense::map<stdx::fixed::string,
                                                              metadata::table,
                                                              stdx::string_transparent_hash,
                                                              stdx::string_transparent_eq>;
    using tables_by_id_map_t   = ankerl::unordered_dense::
        map<table_id_t, gsl::not_null<const metadata::table*>, stdx::hash<table_id_t>>;
    using indexes_by_name_map_t = ankerl::unordered_dense::map<stdx::fixed::string,
                                                               metadata::index,
                                                               stdx::string_transparent_hash,
                                                               stdx::string_transparent_eq>;
    using indexes_by_id_map_t   = ankerl::unordered_dense::
        map<index_id_t, gsl::not_null<const metadata::index*>, stdx::hash<index_id_t>>;

  public:
    catalog(pool_t&                            pool,
            txn::manager&                      txn_mgr,
            txn::undo::manager<i64, PoolSize>& undo_mgr,
            wal::log::manager&                 log_mgr) noexcept
        : pool_{pool}, txn_mgr_{txn_mgr}, undo_mgr_{undo_mgr}, log_mgr_{log_mgr} {}

    [[nodiscard]] auto bootstrap() -> result<void> {
        std::unique_lock lock{mutex_};

        bool needs_init{false};
        if (pool_.num_pages() == 0) {
            needs_init = true;
        } else {
            if (auto read_res{pool_.fetch_read(storage::page_id_t{0})}; !read_res) {
                needs_init = true;
            } else {
                auto       guard{std::move(*read_res)};
                const auto header{guard.template as<metadata::db_header_t>()};
                if (header->magic != metadata::sys_header_magic) {
                    needs_init = true;
                } else {
                    sys_tables_root_  = storage::page_id_t{header->sys_tables_root};
                    sys_columns_root_ = storage::page_id_t{header->sys_columns_root};
                    sys_indexes_root_ = storage::page_id_t{header->sys_indexes_root};
                }
            }
        }

        if (needs_init) {
            const auto pg0{pool_.num_pages() == 0 ? TRY(pool_.new_page())
                                                  : TRY(pool_.fetch_page(storage::page_id_t{0}))};

            pg0->latch().lock();
            typename pool_t::write_guard_t guard0{pool_, *pg0};
            guard0.mark_dirty();
            auto header{guard0.template as<metadata::db_header_t>()};
            header->magic = metadata::sys_header_magic;

            auto sys_tables_tree_base{TRY(txn_tree_t::tree_t::create(pool_))};
            auto sys_columns_tree_base{TRY(txn_tree_t::tree_t::create(pool_))};
            auto sys_indexes_tree_base{TRY(txn_tree_t::tree_t::create(pool_))};

            sys_tables_root_  = sys_tables_tree_base.meta_page();
            sys_columns_root_ = sys_columns_tree_base.meta_page();
            sys_indexes_root_ = sys_indexes_tree_base.meta_page();

            header->sys_tables_root  = std::to_underlying(sys_tables_root_);
            header->sys_columns_root = std::to_underlying(sys_columns_root_);
            header->sys_indexes_root = std::to_underlying(sys_indexes_root_);

            const auto bootstrap_txn{txn_mgr_.begin_txn()};
            txn_tree_t sys_tables_txn_tree{sys_tables_tree_base, undo_mgr_};
            txn_tree_t sys_columns_txn_tree{sys_columns_tree_base, undo_mgr_};

            TRY(insert_table_metadata_txn(bootstrap_txn,
                                          sys_tables_txn_tree,
                                          sys_columns_txn_tree,
                                          table_id_t{1},
                                          metadata::sys_tables_name,
                                          sys_tables_root_,
                                          metadata::sys_tables_schema()));
            TRY(insert_table_metadata_txn(bootstrap_txn,
                                          sys_tables_txn_tree,
                                          sys_columns_txn_tree,
                                          table_id_t{2},
                                          metadata::sys_columns_name,
                                          sys_columns_root_,
                                          metadata::sys_columns_schema()));
            TRY(insert_table_metadata_txn(bootstrap_txn,
                                          sys_tables_txn_tree,
                                          sys_columns_txn_tree,
                                          table_id_t{3},
                                          metadata::sys_indexes_name,
                                          sys_indexes_root_,
                                          metadata::sys_indexes_schema()));

            TRY(pool_.flush());
            TRY(txn_mgr_.update_txn_lsn(bootstrap_txn, wal::log::seq_num{1}));
            TRY(txn_mgr_.commit_txn(bootstrap_txn, log_mgr_));
        }

        TRY(load_catalog_cache_locked());
        return {};
    }

    [[nodiscard]] auto sys_tables_root() const noexcept -> storage::page_id_t {
        std::shared_lock lock{mutex_};
        return sys_tables_root_;
    }

    [[nodiscard]] auto sys_columns_root() const noexcept -> storage::page_id_t {
        std::shared_lock lock{mutex_};
        return sys_columns_root_;
    }

    [[nodiscard]] auto sys_indexes_root() const noexcept -> storage::page_id_t {
        std::shared_lock lock{mutex_};
        return sys_indexes_root_;
    }

    [[nodiscard]] auto get_table(std::string_view name) const
        -> stdx::option<const metadata::table&> {
        std::shared_lock lock{mutex_};
        auto             it{tables_by_name_.find(name)};
        if (it == tables_by_name_.end()) { return stdx::none; }
        return it->second;
    }

    [[nodiscard]] auto get_table(table_id_t table_id) const
        -> stdx::option<const metadata::table&> {
        std::shared_lock lock{mutex_};
        auto             it{tables_by_id_.find(table_id)};
        if (it == tables_by_id_.end()) { return stdx::none; }
        return *it->second;
    }

    [[nodiscard]] auto get_index(std::string_view name) const
        -> stdx::option<const metadata::index&> {
        std::shared_lock lock{mutex_};
        auto             it{indexes_by_name_.find(name)};
        if (it == indexes_by_name_.end()) { return stdx::none; }
        return it->second;
    }

    [[nodiscard]] auto get_index(index_id_t index_id) const
        -> stdx::option<const metadata::index&> {
        std::shared_lock lock{mutex_};
        auto             it{indexes_by_id_.find(index_id)};
        if (it == indexes_by_id_.end()) { return stdx::none; }
        return *it->second;
    }

    [[nodiscard]] auto
    create_table(txn::id_t txn_id, table_id_t table_id, std::string_view name, const schema& sch)
        -> result<metadata::table> {
        std::unique_lock lock{mutex_};
        if (tables_by_name_.contains(name) || tables_by_id_.contains(table_id)) {
            return stdx::err{error::SQL_TABLE_ALREADY_EXISTS};
        }

        auto       table_tree_base{TRY(txn_tree_t::tree_t::create(pool_))};
        const auto root_page_id{table_tree_base.meta_page()};

        typename txn_tree_t::tree_t sys_tables_tree_impl{pool_, sys_tables_root_};
        txn_tree_t                  sys_tables_tree{sys_tables_tree_impl, undo_mgr_};
        typename txn_tree_t::tree_t sys_columns_tree_impl{pool_, sys_columns_root_};
        txn_tree_t                  sys_columns_tree{sys_columns_tree_impl, undo_mgr_};

        TRY(insert_table_metadata_txn(
            txn_id, sys_tables_tree, sys_columns_tree, table_id, name, root_page_id, sch));

        auto inserted{tables_by_name_.emplace(name,
                                              metadata::table{
                                                  .table_id     = table_id,
                                                  .name         = stdx::fixed::string{name},
                                                  .root_page_id = root_page_id,
                                                  .table_schema = sch,
                                              })};
        tables_by_id_.emplace(table_id, &inserted.first->second);
        return inserted.first->second;
    }

    [[nodiscard]] auto drop_table(txn::id_t txn_id, std::string_view name) -> result<void> {
        std::unique_lock lock{mutex_};

        auto it{tables_by_name_.find(name)};
        if (it == tables_by_name_.end()) { return stdx::err{error::SQL_TABLE_NOT_FOUND}; }

        const auto table_id{it->second.table_id};
        const auto root_page_id{it->second.root_page_id};

        typename txn_tree_t::tree_t sys_tables_tree_impl{pool_, sys_tables_root_};
        txn_tree_t                  sys_tables_tree{sys_tables_tree_impl, undo_mgr_};
        typename txn_tree_t::tree_t sys_columns_tree_impl{pool_, sys_columns_root_};
        txn_tree_t                  sys_columns_tree{sys_columns_tree_impl, undo_mgr_};

        TRY(sys_tables_tree.delete_txn(txn_id, std::to_underlying(table_id)));
        for (usize i{0}; i < it->second.table_schema.column_count(); ++i) {
            const i64 composite_id{(std::to_underlying(table_id) << 32) | static_cast<i64>(i)};
            TRY(sys_columns_tree.delete_txn(txn_id, composite_id));
        }

        typename txn_tree_t::tree_t table_tree{pool_, root_page_id};
        TRY(table_tree.delete_tree_pages());

        tables_by_id_.erase(table_id);
        tables_by_name_.erase(it);
        return {};
    }

    [[nodiscard]] auto create_index(txn::id_t           txn_id,
                                    table_id_t          table_id,
                                    index_id_t          index_id,
                                    stdx::fixed::string name,
                                    i32                 column_id,
                                    bool                is_unique) -> result<metadata::index> {
        std::unique_lock lock{mutex_};

        if (indexes_by_name_.contains(name) || indexes_by_id_.contains(index_id)) {
            return stdx::err{error::SQL_INDEX_ALREADY_EXISTS};
        }

        auto       index_tree_base{TRY(txn_tree_t::tree_t::create(pool_))};
        const auto root_page_id{index_tree_base.meta_page()};

        typename txn_tree_t::tree_t sys_indexes_tree_impl{pool_, sys_indexes_root_};
        txn_tree_t                  sys_indexes_tree{sys_indexes_tree_impl, undo_mgr_};

        const i64 composite_id{(std::to_underlying(table_id) << 32) | std::to_underlying(index_id)};
        std::vector<value_t> idx_vals{value_t{composite_id},
                                      value_t{name.view()},
                                      value_t{column_id},
                                      value_t{static_cast<i64>(std::to_underlying(root_page_id))},
                                      value_t{is_unique}};
        auto idx_tuple{TRY(tuple::serialize(metadata::sys_indexes_schema(), idx_vals))};
        TRY(sys_indexes_tree.insert_txn(txn_id, composite_id, idx_tuple.data()));

        auto inserted{indexes_by_name_.emplace(name,
                                               metadata::index{
                                                   .table_id     = table_id,
                                                   .index_id     = index_id,
                                                   .name         = name,
                                                   .column_id    = column_id,
                                                   .root_page_id = root_page_id,
                                                   .is_unique    = is_unique,
                                               })};
        indexes_by_id_.emplace(index_id, &inserted.first->second);
        return inserted.first->second;
    }

    [[nodiscard]] auto drop_index(txn::id_t txn_id, std::string_view name) -> result<void> {
        std::unique_lock lock{mutex_};

        auto it{indexes_by_name_.find(name)};
        if (it == indexes_by_name_.end()) { return stdx::err{error::SQL_INDEX_NOT_FOUND}; }

        const auto table_id{it->second.table_id};
        const auto index_id{it->second.index_id};
        const auto root_page_id{it->second.root_page_id};

        typename txn_tree_t::tree_t sys_indexes_tree_impl{pool_, sys_indexes_root_};
        txn_tree_t                  sys_indexes_tree{sys_indexes_tree_impl, undo_mgr_};

        const i64 composite_id{(std::to_underlying(table_id) << 32) | std::to_underlying(index_id)};
        TRY(sys_indexes_tree.delete_txn(txn_id, composite_id));

        typename txn_tree_t::tree_t index_tree{pool_, root_page_id};
        TRY(index_tree.delete_tree_pages());

        indexes_by_id_.erase(index_id);
        indexes_by_name_.erase(it);
        return {};
    }

    [[nodiscard]] auto get_table_indexes(table_id_t table_id) const
        -> std::vector<metadata::index> {
        std::shared_lock             lock{mutex_};
        std::vector<metadata::index> res;
        for (const auto& [_, idx] : indexes_by_name_) {
            if (idx.table_id == table_id) { res.emplace_back(idx); }
        }
        return res;
    }

    [[nodiscard]] auto alter_table_schema(txn::id_t     txn_id,
                                          table_id_t    table_id,
                                          const schema& new_sch) -> result<void> {
        std::unique_lock lock{mutex_};
        auto             it{tables_by_id_.find(table_id)};
        if (it == tables_by_id_.end()) { return stdx::err{error::SQL_TABLE_NOT_FOUND}; }
        const auto& tbl{*it->second};

        // Delete old columns from sys_columns tree
        typename txn_tree_t::tree_t sys_columns_tree_impl{pool_, sys_columns_root_};
        txn_tree_t                  sys_columns_tree{sys_columns_tree_impl, undo_mgr_};

        for (usize i{0}; i < tbl.table_schema.column_count(); ++i) {
            const i64 composite_id{(std::to_underlying(table_id) << 32) | static_cast<i64>(i)};
            TRY(sys_columns_tree.delete_txn(txn_id, composite_id));
        }

        // Insert new columns into sys_columns tree
        for (usize i{0}; i < new_sch.column_count(); ++i) {
            const auto& col{new_sch[i]};
            const i64   composite_id{(std::to_underlying(table_id) << 32) | static_cast<i64>(i)};
            std::vector<value_t> col_vals{value_t{composite_id},
                                          value_t{col.name()},
                                          value_t{static_cast<i32>(col.type())},
                                          value_t{col.nullable()}};
            auto col_tuple{TRY(tuple::serialize(metadata::sys_columns_schema(), col_vals))};
            TRY(sys_columns_tree.insert_txn(txn_id, composite_id, col_tuple.data()));
        }

        // Update in cache
        if (auto name_it{tables_by_name_.find(tbl.name)}; name_it != tables_by_name_.end()) {
            name_it->second.table_schema = new_sch;
        }
        return {};
    }

    [[nodiscard]] auto update_index_column_id(txn::id_t  txn_id,
                                              index_id_t index_id,
                                              i32        new_column_id) -> result<void> {
        std::unique_lock lock{mutex_};
        auto             it{indexes_by_id_.find(index_id)};
        if (it == indexes_by_id_.end()) { return stdx::err{error::SQL_INDEX_NOT_FOUND}; }
        const auto& idx{*it->second};

        // Update in sys_indexes B+ tree
        typename txn_tree_t::tree_t sys_indexes_tree_impl{pool_, sys_indexes_root_};
        txn_tree_t                  sys_indexes_tree{sys_indexes_tree_impl, undo_mgr_};

        const i64            composite_id{(std::to_underlying(idx.table_id) << 32) |
                               std::to_underlying(index_id)};
        std::vector<value_t> idx_vals{
            value_t{composite_id},
            value_t{idx.name.view()},
            value_t{new_column_id},
            value_t{static_cast<i64>(std::to_underlying(idx.root_page_id))},
            value_t{idx.is_unique}};
        auto idx_tuple{TRY(tuple::serialize(metadata::sys_indexes_schema(), idx_vals))};
        TRY(sys_indexes_tree.update_txn(txn_id, composite_id, idx_tuple.data()));

        // Update in cache
        if (auto name_it{indexes_by_name_.find(idx.name)}; name_it != indexes_by_name_.end()) {
            name_it->second.column_id = new_column_id;
        }
        return {};
    }

    [[nodiscard]] auto undo_manager() noexcept -> txn::undo::manager<i64, PoolSize>& {
        return undo_mgr_;
    }

    [[nodiscard]] auto get_next_table_id() const noexcept -> table_id_t {
        std::shared_lock lock{mutex_};
        return get_next_id<table_id_t>(tables_by_id_);
    }

    [[nodiscard]] auto get_next_index_id() const noexcept -> index_id_t {
        std::shared_lock lock{mutex_};
        return get_next_id<index_id_t>(indexes_by_id_);
    }

  private:
    struct table_row_t {
        table_id_t          id;
        stdx::fixed::string name;
        storage::page_id_t  root_page_id;
    };

  private:
    template <typename Id>
    [[nodiscard]] static auto get_next_id(const auto& by_id_table) noexcept -> Id {
        if (by_id_table.empty()) { return Id{1}; }
        auto max_id{std::ranges::max(by_id_table, {}, [](const auto& entry) {
                        return std::to_underlying(entry.first);
                    }).first};
        using namespace stdx::enum_ops;
        return ++max_id;
    }

    [[nodiscard]] auto insert_table_metadata_txn(txn::id_t          txn_id,
                                                 txn_tree_t&        sys_tables_tree,
                                                 txn_tree_t&        sys_columns_tree,
                                                 table_id_t         table_id,
                                                 std::string_view   name,
                                                 storage::page_id_t root_page,
                                                 const schema&      sch) -> result<void> {
        std::vector<value_t> table_vals{value_t{std::to_underlying(table_id)},
                                        value_t{name},
                                        value_t{static_cast<i64>(std::to_underlying(root_page))}};
        auto table_tuple = TRY(tuple::serialize(metadata::sys_tables_schema(), table_vals));
        TRY(sys_tables_tree.insert_txn(txn_id, std::to_underlying(table_id), table_tuple.data()));

        for (usize i{0}; i < sch.column_count(); ++i) {
            const auto& col{sch[i]};
            const i64   composite_id{(std::to_underlying(table_id) << 32) | static_cast<i64>(i)};
            std::vector<value_t> col_vals{value_t{composite_id},
                                          value_t{col.name()},
                                          value_t{static_cast<i32>(col.type())},
                                          value_t{col.nullable()}};
            auto col_tuple{TRY(tuple::serialize(metadata::sys_columns_schema(), col_vals))};
            TRY(sys_columns_tree.insert_txn(txn_id, composite_id, col_tuple.data()));
        }
        return {};
    }

    [[nodiscard]] auto load_catalog_cache_locked() -> result<void> {
        tables_by_name_.clear();
        tables_by_id_.clear();
        indexes_by_name_.clear();
        indexes_by_id_.clear();

        const auto scan_txn{txn_mgr_.begin_txn()};
        const auto snap{TRY(txn_mgr_.acquire_snapshot(scan_txn))};

        typename txn_tree_t::tree_t sys_tables_tree_impl{pool_, sys_tables_root_};
        txn_tree_t                  sys_tables_tree{sys_tables_tree_impl, undo_mgr_};
        typename txn_tree_t::tree_t sys_columns_tree_impl{pool_, sys_columns_root_};
        txn_tree_t                  sys_columns_tree{sys_columns_tree_impl, undo_mgr_};
        typename txn_tree_t::tree_t sys_indexes_tree_impl{pool_, sys_indexes_root_};
        txn_tree_t                  sys_indexes_tree{sys_indexes_tree_impl, undo_mgr_};

        i64 low{0};
        i64 high{std::numeric_limits<i64>::max()};

        catalog_scan_t           tables_scan{sys_tables_tree, scan_txn, snap, txn_mgr_};
        catalog_scan_t           columns_scan{sys_columns_tree, scan_txn, snap, txn_mgr_};
        catalog_scan_t           indexes_scan{sys_indexes_tree, scan_txn, snap, txn_mgr_};
        std::vector<table_row_t> table_rows;

        TRY(tables_scan(low, high, [&](const i64& key, gsl::span<const std::byte> val) -> bool {
            tuple::byte_buffer buf;
            buf.resize(val.size());
            std::memcpy(buf.data(), val.data(), val.size());
            tuple t{std::move(buf)};

            std::array<value_t, 3> vals;
            if (!t.deserialize(metadata::sys_tables_schema(), vals)) { return false; }

            auto name_sv{vals[1].get_value().as<std::string_view>()};
            table_rows.emplace_back(static_cast<table_id_t>(key),
                                    stdx::fixed::string{name_sv},
                                    storage::page_id_t{vals[2].get_value().as<i64>()});
            return true;
        }));

        for (const auto& row : table_rows) {
            i64 col_low{std::to_underlying(row.id) << 32};
            i64 col_high{col_low | 0xFFFFFFFFLL};

            std::vector<column> cols;
            TRY(columns_scan(
                col_low, col_high, [&](const i64&, gsl::span<const std::byte> val) -> bool {
                    tuple::byte_buffer buf;
                    buf.resize(val.size());
                    std::memcpy(buf.data(), val.data(), val.size());
                    tuple t{std::move(buf)};

                    std::array<value_t, 4> vals;
                    if (!t.deserialize(metadata::sys_columns_schema(), vals)) { return false; }

                    auto col_name_sv{vals[1].get_value().as<std::string_view>()};
                    auto type_id{static_cast<type::id_t>(vals[2].get_value().as<i32>())};
                    bool nullable{vals[3].get_value().as<bool>()};

                    cols.emplace_back(stdx::fixed::string{col_name_sv}, type_id, nullable);
                    return true;
                }));

            schema table_sch{std::move(cols)};
            auto   inserted{tables_by_name_.emplace(row.name,
                                                  metadata::table{
                                                        .table_id     = row.id,
                                                        .name         = row.name,
                                                        .root_page_id = row.root_page_id,
                                                        .table_schema = std::move(table_sch),
                                                  })};
            tables_by_id_.emplace(row.id, &inserted.first->second);
        }

        TRY(indexes_scan(low, high, [&](const i64& key, gsl::span<const std::byte> val) -> bool {
            tuple::byte_buffer buf;
            buf.resize(val.size());
            std::memcpy(buf.data(), val.data(), val.size());
            tuple t{std::move(buf)};

            std::array<value_t, 5> vals;
            if (!t.deserialize(metadata::sys_indexes_schema(), vals)) { return false; }

            auto table_id{static_cast<table_id_t>(key >> 32)};
            auto index_id{static_cast<index_id_t>(key & 0xFFFFFFFFLL)};
            auto idx_name{vals[1].get_value().as<std::string_view>()};
            auto column_id{vals[2].get_value().as<i32>()};
            auto root_page_id{storage::page_id_t{vals[3].get_value().as<i64>()}};
            auto is_unique{vals[4].get_value().as<bool>()};

            auto inserted{indexes_by_name_.emplace(idx_name,
                                                   metadata::index{
                                                       .table_id  = table_id,
                                                       .index_id  = index_id,
                                                       .name      = stdx::fixed::string{idx_name},
                                                       .column_id = column_id,
                                                       .root_page_id = root_page_id,
                                                       .is_unique    = is_unique,
                                                   })};
            indexes_by_id_.emplace(index_id, &inserted.first->second);
            return true;
        }));

        return txn_mgr_.abort_txn(scan_txn, log_mgr_);
    }

  private:
    pool_t&                            pool_;
    txn::manager&                      txn_mgr_;
    txn::undo::manager<i64, PoolSize>& undo_mgr_;
    wal::log::manager&                 log_mgr_;

    mutable std::shared_mutex mutex_;
    storage::page_id_t        sys_tables_root_{storage::INVALID_PAGE_ID};
    storage::page_id_t        sys_columns_root_{storage::INVALID_PAGE_ID};
    storage::page_id_t        sys_indexes_root_{storage::INVALID_PAGE_ID};

    tables_by_name_map_t  tables_by_name_;
    tables_by_id_map_t    tables_by_id_;
    indexes_by_name_map_t indexes_by_name_;
    indexes_by_id_map_t   indexes_by_id_;
};

} // namespace cairn::sql
