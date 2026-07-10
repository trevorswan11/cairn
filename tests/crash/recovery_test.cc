#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <gsl/span>
#include <magic_enum/magic_enum.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/crash/injection.hh"
#include "testhelpers/argv.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/mt_verifier.hh"
#include "testhelpers/subprocess.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/manager.hh"
#include "wal/log/manager.hh"
#include "wal/log/reader.hh"
#include "wal/log/record.hh"
#include "wal/recovery/manager.hh"

namespace cairn::tests {

using namespace stdx::size_literals;

TEST_CASE("crash recovery workload", "[.][crash]") {
    const auto db_path{helpers::get_env("CAIRN_DB_PATH")};
    const auto wal_path{helpers::get_env("CAIRN_WAL_PATH")};
    const auto limit_str{helpers::get_env("CAIRN_CRASH_LIMIT")};

    const auto limit{helpers::parse_integral<i32>(limit_str)};
    crash::initialize();
    crash::configure(limit);

    using pool_t = storage::buffer_pool<8>;
    wal::log::manager log{wal_path, 4_KiB};
    auto              bp{UNWRAP(pool_t::open(db_path))};
    bp->set_log_manager(log);
    txn::manager tm;

    // Concurrent writers executing transactions
    std::vector<std::jthread> writers;
    std::atomic<bool>         go{false};
    helpers::mt_verifier      verifier;

    for (i32 t{0}; t < 2; ++t) {
        writers.emplace_back([&bp, &log, &tm, t, &go, &verifier] {
            while (!go.load()) { std::this_thread::yield(); }

            for (i32 i{0}; i < 3; ++i) {
                const auto         tid{tm.begin_txn()};
                storage::page_id_t pid;
                {
                    auto [id, guard]{MT_UNWRAP(verifier, bp->new_write())};
                    pid = id;
                    storage::slotted_page sp{*guard.get()};
                    sp.refresh_page();
                    guard.mark_dirty();
                }

                {
                    auto                  guard{MT_UNWRAP(verifier, bp->fetch_write(pid))};
                    storage::slotted_page sp{*guard.get()};

                    const auto data{fmt::format("t_{}_s_{}", t, i)};
                    THREAD_CHECK(verifier,
                                 sp.insert(helpers::span_from_string(data),
                                           {
                                               .txn_id      = tid,
                                               .prev_lsn    = stdx::none,
                                               .log_manager = log,
                                           }));

                    if (auto page_lsn{guard.get()->page_lsn()}) {
                        THREAD_CHECK(verifier, tm.update_txn_lsn(tid, *page_lsn));
                    }
                    guard.mark_dirty();
                }

                THREAD_CHECK(verifier, tm.commit_txn(tid, log));
            }
        });
    }

    go.store(true);
    writers.clear();
    REQUIRE_FALSE(verifier.dump_if_error());
}

namespace {

struct update_t {
    storage::page_id_t pid;
    storage::slot_id_t sid;
    std::string        data;
};
using update_map_t = ankerl::unordered_dense::map<txn::id_t, std::vector<update_t>, txn::id_hash_t>;

} // namespace

TEST_CASE("crash recovery entrypoint") {
    const auto self_exe{helpers::self_exe_path()};
    i32        limit{1};
    bool       finished{false};

    while (!finished) {
        helpers::tempfile db_file{"crash_recovery_db"};
        helpers::set_env("CAIRN_DB_PATH", db_file.path.string());
        helpers::tempfile log_file{"crash_recovery_log"};
        helpers::set_env("CAIRN_WAL_PATH", log_file.path.string());
        helpers::tempfile control_file{"crash_recovery_control"};

        helpers::set_env("CAIRN_CRASH_LIMIT", std::to_string(limit));
        const helpers::Argv args{self_exe, "[crash]"};
        const auto          exit_code{UNWRAP(helpers::spawn_child(args))};

        if (exit_code == 0) {
            // Workload ran to completion without hitting the crash limit
            finished = true;
        } else {
            // Child crashed! Run recovery and verify consistency
            using pool_t = storage::buffer_pool<8>;

            {
                wal::log::manager log{log_file.path, 4_KiB};
                auto              bp{UNWRAP(pool_t::open(db_file.path))};
                bp->set_log_manager(log);

                txn::manager              tm;
                wal::recovery::manager<8> rm{*bp, tm, log, control_file.path, log_file.path};
                REQUIRE(rm.recover());
            }

            // Check correctness of recovered state by scanning the WAL
            auto reader{UNWRAP(wal::log::reader::open(log_file.path))};
            REQUIRE(reader.seek_to_start());

            ankerl::unordered_dense::set<txn::id_t, txn::id_hash_t> committed_txns;
            update_map_t                                            updates;

            while (auto rec{UNWRAP(reader.next_record_lenient())}) {
                if (rec->type == wal::log::record_type::COMMIT) {
                    committed_txns.insert(rec->txn_id);
                } else if (rec->type == wal::log::record_type::UPDATE) {
                    updates[rec->txn_id].emplace_back(
                        UNWRAP(rec->page_id),
                        UNWRAP(rec->slot_id),
                        std::string{helpers::string_from_span(rec->redo_data)});
                }
            }

            // Verify recovered database pages match the committed txn state
            auto      bp{UNWRAP(pool_t::open(db_file.path))};
            const i64 num_pages{bp->num_pages()};

            for (const auto& [tid, tx_updates] : updates) {
                const bool is_committed{committed_txns.contains(tid)};

                for (const auto& upd : tx_updates) {
                    const auto page_id_val{std::to_underlying(upd.pid)};

                    if (is_committed) {
                        REQUIRE(page_id_val < num_pages);
                        auto                  guard{UNWRAP(bp->fetch_read(upd.pid))};
                        storage::slotted_page sp{*guard.get()};
                        auto                  tuple{UNWRAP(sp.get(upd.sid))};
                        CHECK(helpers::string_from_span(tuple) == upd.data);
                    } else {
                        if (page_id_val < num_pages) {
                            if (auto guard{bp->fetch_read(upd.pid)}) {
                                storage::slotted_page sp{*guard.value().get()};
                                auto                  tuple_res{sp.get(upd.sid)};
                                if (tuple_res) {
                                    CHECK(helpers::string_from_span(tuple_res.value()) != upd.data);
                                }
                            }
                        }
                    }
                }
            }

            // Holy indentation
            limit++;
        }
    }
}

} // namespace cairn::tests
