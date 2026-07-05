#pragma once

#include <algorithm>
#include <filesystem>
#include <stdx/profiler.hh>
#include <utility>

#include <ankerl/unordered_dense.h>
#include <stdx/enum.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/buffer_pool.hh"
#include "storage/page.hh"
#include "storage/slotted_page.hh"
#include "support/error.hh"
#include "txn/id.hh"
#include "txn/manager.hh"
#include "wal/checkpoint/manager.hh"
#include "wal/checkpoint/types.hh"
#include "wal/log/manager.hh"
#include "wal/log/reader.hh"
#include "wal/log/record.hh"
#include "wal/log/seq_num.hh"

namespace cairn::wal::recovery {

template <usize PoolSize> class manager {
  public:
    manager(storage::buffer_pool<PoolSize>& pool,
            txn::manager&                   tm,
            log::manager&                   log_manager,
            std::filesystem::path           control_path,
            std::filesystem::path           log_path) noexcept
        : pool_{pool}, tm_{tm}, log_manager_{log_manager}, control_path_{std::move(control_path)},
          log_path_{std::move(log_path)}, cm_{control_path_} {}

    [[nodiscard]] auto recover() -> result<void> {
        PROFILE_FUNCTION();
        using namespace stdx::enum_ops;
        auto analysis_res{TRY(run_analysis())};
        if (analysis_res.max_lsn) { log_manager_.set_lsn_watermarks(*analysis_res.max_lsn); }
        tm_.set_next_txn_id(++analysis_res.max_txn_id);

        TRY(run_redo(analysis_res.dirty_pages));
        TRY(run_undo(analysis_res.active_txns));
        return {};
    }

  private:
    using active_txn_map_t =
        ankerl::unordered_dense::map<txn::id_t, checkpoint::att_entry, txn::id_hash_t>;
    using dirty_page_map_t =
        ankerl::unordered_dense::map<storage::page_id_t, log::seq_num, storage::page_id_hash_t>;
    using to_undo_map_t = ankerl::unordered_dense::map<txn::id_t, log::seq_num, txn::id_hash_t>;

    struct analysis_data {
        active_txn_map_t           active_txns;
        dirty_page_map_t           dirty_pages;
        stdx::option<log::seq_num> max_lsn;
        txn::id_t                  max_txn_id;
    };

  private:
    [[nodiscard]] auto run_analysis() -> result<analysis_data> {
        analysis_data res;

        // Setup should only be fatal if there's an IO error
        stdx::option<log::seq_num> checkpoint_lsn;
        if (auto checkpoint_lsn_res{cm_.read_latest_checkpoint_lsn()}) {
            checkpoint_lsn.emplace(checkpoint_lsn_res.value());
        } else {
            if (checkpoint_lsn_res.error() != error_t::WAL_CONTROL_PATH_NOT_FOUND) {
                return stdx::err{checkpoint_lsn_res.error()};
            }
        }

        auto reader_res{log::reader::open(log_path_)};
        if (!reader_res) {
            if (reader_res.error() == error_t::WAL_LOG_FILE_NOT_FOUND) { return res; }
            return stdx::err{reader_res.error()};
        }
        auto& reader{reader_res.value()};

        // Only seek to the checkpoint if it was able to resolved
        if (checkpoint_lsn) {
            TRY(reader.seek_to_start());
            while (const auto rec{TRY(reader.next_record_lenient())}) {
                res.max_lsn.emplace(rec->lsn);
                res.max_txn_id = std::max(res.max_txn_id, rec->txn_id);
                if (rec->lsn == *checkpoint_lsn) { break; }
            }
        } else {
            TRY(reader.seek_to_start());
        }

        while (const auto rec{TRY(reader.next_record_lenient())}) {
            res.max_lsn.emplace(rec->lsn);
            res.max_txn_id = std::max(res.max_txn_id, rec->txn_id);

            switch (rec->type) {
            case log::record_type::CHECKPOINT_END:
                for (const auto& entry : rec->att) { res.active_txns.emplace(entry.txn_id, entry); }
                for (const auto& entry : rec->dpt) {
                    res.dirty_pages.try_emplace(entry.page_id, entry.rec_lsn);
                }
                break;
            case log::record_type::BEGIN:
                res.active_txns.emplace(rec->txn_id,
                                        checkpoint::att_entry{
                                            .txn_id   = rec->txn_id,
                                            .state    = checkpoint::att_entry::state_t::ACTIVE,
                                            .last_lsn = rec->lsn,
                                        });
                break;
            case log::record_type::UPDATE:
            case log::record_type::CLEAR:  {
                auto emplace{
                    res.active_txns.try_emplace(rec->txn_id,
                                                checkpoint::att_entry{
                                                    .txn_id = rec->txn_id,
                                                    .state = checkpoint::att_entry::state_t::ACTIVE,
                                                    .last_lsn = rec->lsn,
                                                })};

                if (!emplace.second) { emplace.first->second.last_lsn = rec->lsn; }
                if (rec->page_id) { res.dirty_pages.try_emplace(*rec->page_id, rec->lsn); }
                break;
            }
            case log::record_type::COMMIT:
            case log::record_type::ABORT:  {
                res.active_txns.erase(rec->txn_id);
                break;
            }
            case log::record_type::CHECKPOINT_BEGIN: break;
            }
        }

        return res;
    }

    [[nodiscard]] auto run_undo(active_txn_map_t& att) -> result<void> {
        if (att.empty()) { return {}; }
        auto reader{TRY(log::reader::open(log_path_))};
        TRY(reader.seek_to_end());

        to_undo_map_t to_undo;
        for (const auto& [tid, entry] : att) {
            if (entry.last_lsn) { to_undo.emplace(tid, *entry.last_lsn); }
        }

        stdx::option<log::seq_num> last_appended_lsn;
        while (auto prev_pos{TRY(reader.has_prev())}) {
            if (to_undo.empty()) { break; }
            const auto rec{TRY(reader.prev_at(*prev_pos))};

            const auto txn_id_it{to_undo.find(rec.txn_id)};
            if (txn_id_it == to_undo.end()) { continue; }
            if (txn_id_it->second != rec.lsn) { continue; }

            const auto att_it{att.find(rec.txn_id)};
            if (att_it == att.end()) { continue; }
            auto& rec_txn_id{att_it->second};

            switch (rec.type) {
            case log::record_type::UPDATE:
                if (rec.page_id) {
                    const auto pid{*rec.page_id};
                    auto       guard{TRY(pool_.fetch_write(pid))};

                    storage::slotted_page sp{*guard.get()};
                    if (!rec.undo_data.empty()) {
                        TRY(sp.write_slot_raw(*rec.slot_id, rec.undo_data));
                    } else {
                        TRY(sp.write_slot_raw(*rec.slot_id, stdx::none));
                    }

                    log::record clr;
                    clr.txn_id        = rec.txn_id;
                    clr.type          = log::record_type::CLEAR;
                    clr.page_id       = rec.page_id;
                    clr.slot_id       = rec.slot_id;
                    clr.prev_lsn      = rec_txn_id.last_lsn;
                    clr.undo_next_lsn = rec.prev_lsn;
                    clr.redo_data     = rec.undo_data;

                    const auto clr_lsn = TRY(log_manager_.append_record(clr));
                    last_appended_lsn.emplace(clr_lsn);

                    guard.get()->set_page_lsn(clr_lsn);
                    guard.mark_dirty();
                    rec_txn_id.last_lsn = clr_lsn;
                }

                if (rec.prev_lsn) {
                    txn_id_it->second = *rec.prev_lsn;
                } else {
                    log::record abort_rec;
                    abort_rec.txn_id   = rec.txn_id;
                    abort_rec.type     = log::record_type::ABORT;
                    abort_rec.prev_lsn = rec_txn_id.last_lsn;

                    const auto abort_lsn{TRY(log_manager_.append_record(abort_rec))};
                    last_appended_lsn.emplace(abort_lsn);

                    to_undo.erase(txn_id_it);
                    att.erase(att_it);
                }
                break;
            case log::record_type::CLEAR:
                if (rec.undo_next_lsn) {
                    txn_id_it->second = *rec.undo_next_lsn;
                } else {
                    log::record abort_rec;
                    abort_rec.txn_id   = rec.txn_id;
                    abort_rec.type     = log::record_type::ABORT;
                    abort_rec.prev_lsn = rec_txn_id.last_lsn;

                    const auto abort_lsn{TRY(log_manager_.append_record(abort_rec))};
                    last_appended_lsn.emplace(abort_lsn);

                    to_undo.erase(txn_id_it);
                    att.erase(att_it);
                }
                break;
            case log::record_type::BEGIN: {
                log::record abort_rec;
                abort_rec.txn_id   = rec.txn_id;
                abort_rec.type     = log::record_type::ABORT;
                abort_rec.prev_lsn = rec_txn_id.last_lsn;

                const auto abort_lsn{TRY(log_manager_.append_record(abort_rec))};
                last_appended_lsn.emplace(abort_lsn);

                to_undo.erase(txn_id_it);
                att.erase(att_it);
                break;
            }
            default: break;
            }
        }

        if (last_appended_lsn) { TRY(log_manager_.flush(*last_appended_lsn)); }
        return {};
    }

    [[nodiscard]] auto run_redo(const dirty_page_map_t& dirty_pages) -> result<void> {
        if (dirty_pages.empty()) { return {}; }
        const auto min_rec_lsn{
            std::ranges::min(dirty_pages, {}, &dirty_page_map_t::iterator::value_type::second)
                .second};

        auto reader{TRY(log::reader::open(log_path_))};
        TRY(reader.seek_to_start());

        while (const auto rec{TRY(reader.next_record_lenient())}) {
            if (std::to_underlying(rec->lsn) < std::to_underlying(min_rec_lsn)) { continue; }

            if (rec->type == log::record_type::UPDATE || rec->type == log::record_type::CLEAR) {
                if (!rec->page_id) { continue; }
                const auto pid{*rec->page_id};

                const auto pid_it{dirty_pages.find(pid)};
                if (pid_it == dirty_pages.end()) { continue; }
                if (std::to_underlying(rec->lsn) < std::to_underlying(pid_it->second)) { continue; }

                auto guard{TRY(pool_.fetch_write(pid))};
                auto page_lsn{guard.get()->page_lsn()};
                if (page_lsn && std::to_underlying(*page_lsn) >= std::to_underlying(rec->lsn)) {
                    continue;
                }

                storage::slotted_page sp{*guard.get()};
                if (!rec->redo_data.empty()) {
                    TRY(sp.write_slot_raw(*rec->slot_id, rec->redo_data));
                } else {
                    TRY(sp.write_slot_raw(*rec->slot_id, stdx::none));
                }

                guard.get()->set_page_lsn(rec->lsn);
                guard.mark_dirty();
            }
        }

        return {};
    }

  private:
    storage::buffer_pool<PoolSize>& pool_;
    txn::manager&                   tm_;
    log::manager&                   log_manager_;
    const std::filesystem::path     control_path_;
    const std::filesystem::path     log_path_;
    checkpoint::manager<PoolSize>   cm_;
};

} // namespace cairn::wal::recovery
