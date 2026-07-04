#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
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
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/manager.hh"
#include "wal/checkpoint_manager.hh"
#include "wal/checkpoints.hh"
#include "wal/manager.hh"
#include "wal/reader.hh"
#include "wal/record.hh"
#include "wal/sequence_number.hh"

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
    wal::manager log{log_file.path, 4_KiB};
    auto         bp{helpers::unwrap(pool_t::open(db_file.path))};
    bp->set_log_manager(log);

    txn::manager          tm;
    checkpoint_manager<8> cm{control_file.path};

    const auto tid{tm.begin_txn()};
    page_id_t  pid;
    lsn_t      update_lsn;

    {
        auto [id, guard]{helpers::unwrap(bp->new_write())};
        pid = id;

        slotted_page sp{*guard.get()};
        sp.refresh_page();

        const std::string_view data{"checkpoint test record"};
        CHECK(sp.insert(helpers::span_from_string(data),
                        {
                            .txn_id   = tid,
                            .prev_lsn = stdx::none,
                            .log      = log,
                        }));

        update_lsn = helpers::unwrap(guard.get()->page_lsn());
        REQUIRE(guard.get()->rec_lsn().has_value());
        CHECK(*guard.get()->rec_lsn() == update_lsn);
        REQUIRE(tm.update_txn_lsn(tid, update_lsn));
        guard.mark_dirty();
    }

    const auto cm_lsn{helpers::unwrap(cm.checkpoint(*bp, tm, log))};
    CHECK(cm_lsn != INVALID_LSN);
    const auto persisted_lsn{helpers::unwrap(cm.read_latest_checkpoint_lsn())};
    CHECK(persisted_lsn == cm_lsn);

    {
        auto reader{helpers::unwrap(reader::open(log_file.path))};

        // Record 1: UPDATE
        auto r1{helpers::unwrap(reader.next())};
        CHECK(r1.lsn == update_lsn);
        CHECK(r1.type == record_type::UPDATE);
        CHECK(r1.txn_id == tid);

        // Record 2: CHECKPOINT_BEGIN
        auto r2{helpers::unwrap(reader.next())};
        CHECK(r2.lsn == cm_lsn);
        CHECK(r2.type == record_type::CHECKPOINT_BEGIN);

        // Record 3: CHECKPOINT_END
        auto r3{helpers::unwrap(reader.next())};
        CHECK(r3.type == record_type::CHECKPOINT_END);

        // DPT and ATT details inside CHECKPOINT_END
        REQUIRE(r3.dpt.size() == 1);
        CHECK(r3.dpt[0].page_id == pid);
        CHECK(r3.dpt[0].rec_lsn == update_lsn);

        REQUIRE(r3.att.size() == 1);
        CHECK(r3.att[0].txn_id == tid);
        CHECK(r3.att[0].state == att_state_t::ACTIVE);
        CHECK(r3.att[0].last_lsn == update_lsn);
    }
}

TEST_CASE("checkpoint concurrent with page writes") {
    helpers::tempfile db_file{"checkpoint_concurrent_db"};
    helpers::tempfile log_file{"checkpoint_concurrent_log"};
    helpers::tempfile control_file{"checkpoint_concurrent_control"};

    using pool_t = buffer_pool<8>;
    wal::manager log{log_file.path, 16_KiB};
    auto         bp{helpers::unwrap(pool_t::open(db_file.path))};
    bp->set_log_manager(log);

    txn::manager          tm;
    checkpoint_manager<8> cm{control_file.path};
    const i32             num_threads{4};
    const i32             iterations{20};

    std::vector<std::jthread> writers;
    std::atomic<bool>         stop{false};
    std::atomic<i32>          update_failures{0};

    for (i32 t{0}; t < num_threads; ++t) {
        writers.emplace_back([&bp, &log, &tm, t, &stop, &update_failures]() {
            const auto tid{tm.begin_txn()};

            page_id_t pid;
            {
                auto [id, guard] = helpers::unwrap(bp->new_write());
                pid              = id;
                slotted_page sp{*guard.get()};
                sp.refresh_page();
                guard.mark_dirty();
            }

            i32 seq{0};
            while (!stop.load()) {
                auto         guard{helpers::unwrap(bp->fetch_write(pid))};
                slotted_page sp{*guard.get()};

                const auto data{fmt::format("writer {} seq {}", t, seq++)};
                auto       slot_id{sp.insert(helpers::span_from_string(data),
                                             {
                                                 .txn_id   = tid,
                                                 .prev_lsn = stdx::none,
                                                 .log      = log,
                                       })};
                if (slot_id) {
                    if (auto page_lsn{guard.get()->page_lsn()}) {
                        if (!tm.update_txn_lsn(tid, *page_lsn)) { update_failures += 1; }
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
    CHECK(update_failures == 0);
}

} // namespace cairn::tests
