#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/diagnostic/error.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/mt_verifier.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/manager.hh"
#include "wal/checkpoint/manager.hh"
#include "wal/checkpoint/types.hh"
#include "wal/log/manager.hh"
#include "wal/log/reader.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"

namespace cairn::tests {

using namespace cairn::storage;
using namespace cairn::txn;
using namespace cairn::wal;
using namespace stdx::size_literals;

TEST_CASE("checkpoint basic accuracy") {
    helpers::tempfile db_file{"checkpoint_basic_db"};
    helpers::tempfile log_file{"checkpoint_basic_log"};
    helpers::tempfile control_file{"checkpoint_basic_control"};

    using pool_t = buffer_pool<8>;
    log::manager log{log_file.path, 4_KiB};
    auto         bp{UNWRAP(pool_t::open(db_file.path))};
    bp->set_log_manager(log);

    txn::manager        tm;
    checkpoint::manager cm{control_file.path};

    const auto   tid{tm.begin_txn()};
    page_id_t    pid;
    log::seq_num update_lsn;

    {
        auto [id, guard]{UNWRAP(bp->new_write())};
        pid = id;

        slotted_page sp{*guard.get()};
        sp.refresh_page();

        const std::string_view data{"checkpoint test record"};
        CHECK(sp.insert(helpers::span_from_string(data),
                        {
                            .txn_id      = tid,
                            .prev_lsn    = stdx::none,
                            .log_manager = log,
                        }));

        update_lsn = UNWRAP(guard.get()->page_lsn());
        REQUIRE(guard.get()->rec_lsn().has_value());
        CHECK(*guard.get()->rec_lsn() == update_lsn);
        REQUIRE(tm.update_txn_lsn(tid, update_lsn));
        guard.mark_dirty();
    }

    const auto cm_lsn{UNWRAP(cm.checkpoint(*bp, tm, log))};
    CHECK(cm_lsn != log::INVALID_LSN);
    const auto persisted_lsn{UNWRAP(cm.read_latest_checkpoint_lsn())};
    CHECK(persisted_lsn == cm_lsn);

    {
        auto reader{UNWRAP(log::reader::open(log_file.path))};

        // Record 1: UPDATE
        auto r1{UNWRAP(UNWRAP(reader.next_record()))};
        CHECK(r1.lsn == update_lsn);
        CHECK(r1.type == log::record_type::UPDATE);
        CHECK(r1.txn_id == tid);

        // Record 2: CHECKPOINT_BEGIN
        auto r2{UNWRAP(UNWRAP(reader.next_record()))};
        CHECK(r2.lsn == cm_lsn);
        CHECK(r2.type == log::record_type::CHECKPOINT_BEGIN);

        // Record 3: CHECKPOINT_END
        auto r3{UNWRAP(UNWRAP(reader.next_record()))};
        CHECK(r3.type == log::record_type::CHECKPOINT_END);

        // DPT and ATT details inside CHECKPOINT_END
        REQUIRE(r3.dpt.size() == 1);
        CHECK(r3.dpt[0].page_id == pid);
        CHECK(r3.dpt[0].rec_lsn == update_lsn);

        REQUIRE(r3.att.size() == 1);
        CHECK(r3.att[0].txn_id == tid);
        CHECK(r3.att[0].state == checkpoint::att_entry::state_t::ACTIVE);
        CHECK(r3.att[0].last_lsn == update_lsn);
    }
}

TEST_CASE("checkpoint concurrent with page writes") {
    helpers::tempfile db_file{"checkpoint_concurrent_db"};
    helpers::tempfile log_file{"checkpoint_concurrent_log"};
    helpers::tempfile control_file{"checkpoint_concurrent_control"};

    using pool_t = buffer_pool<8>;
    log::manager log{log_file.path, 16_KiB};
    auto         bp{UNWRAP(pool_t::open(db_file.path))};
    bp->set_log_manager(log);

    txn::manager        tm;
    checkpoint::manager cm{control_file.path};
    const i32           num_threads{4};
    const i32           iterations{20};

    std::vector<std::jthread> writers;
    std::atomic<bool>         stop{false};
    helpers::mt_verifier      verifier;

    for (i32 t{0}; t < num_threads; ++t) {
        writers.emplace_back([&bp, &log, &tm, t, &stop, &verifier] {
            const auto tid{tm.begin_txn()};

            page_id_t pid;
            {
                auto [id, guard] = MT_UNWRAP(verifier, bp->new_write());
                pid              = id;
                slotted_page sp{*guard.get()};
                sp.refresh_page();
                guard.mark_dirty();
            }

            i32 seq{0};
            while (!stop.load()) {
                auto         guard{MT_UNWRAP(verifier, bp->fetch_write(pid))};
                slotted_page sp{*guard.get()};

                const auto data{fmt::format("writer {} seq {}", t, seq++)};
                auto       slot_id{sp.insert(helpers::span_from_string(data),
                                             {
                                                 .txn_id      = tid,
                                                 .prev_lsn    = stdx::none,
                                                 .log_manager = log,
                                       })};
                if (slot_id) {
                    if (auto page_lsn{guard.get()->page_lsn()}) {
                        MT_CHECK(verifier, tm.update_txn_lsn(tid, *page_lsn));
                    }
                    guard.mark_dirty();
                }
                std::this_thread::yield();
            }
        });
    }

    // Run checkpoints in parallel
    for (i32 i{0}; i < iterations; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(cm.checkpoint(*bp, tm, log));
    }

    stop.store(true);
    writers.clear();
    REQUIRE_FALSE(verifier.dump_if_error());
}

TEST_CASE("checkpoint read_latest_checkpoint_lsn error paths") {
    helpers::tempfile   control_file{"checkpoint_err_control"};
    checkpoint::manager cm{control_file.path};
    CHECK(UNWRAP_ERR(cm.read_latest_checkpoint_lsn()) == error::WAL_CONTROL_PATH_NOT_FOUND);

    // Corrupted / invalid size file
    {
        std::ofstream ofs{control_file.path, std::ios::binary};
        ofs << "short";
    }
    CHECK(UNWRAP_ERR(cm.read_latest_checkpoint_lsn()) == error::IO_ERROR);
}

} // namespace cairn::tests
