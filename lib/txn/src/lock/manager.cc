#include "txn/lock/manager.hh"

#include <algorithm>
#include <mutex>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/error.hh"
#include "txn/id.hh"
#include "txn/lock/types.hh"

namespace cairn::txn::lock {

auto manager::release_all_locks(id_t id) -> void {
    std::lock_guard lock{mutex_};

    wounded_txns_.remove(id);
    const auto txn_res_opt{tracked_txn_resources_.get_opt(id)};
    if (!txn_res_opt) { return; }
    const auto& txn_res{*txn_res_opt};

    for (const auto& res_id : txn_res) {
        const auto bucket{bucket_table_.get_opt(res_id)};
        if (!bucket) { continue; }

        // Remove all requests for this transaction
        auto& reqs{bucket->requests};
        for (auto it{reqs.begin()}; it != reqs.end();) {
            if (it->txn_id == id) {
                it = reqs.erase(it);
            } else {
                ++it;
            }
        }

        if (reqs.empty()) {
            bucket_table_.remove(res_id);
            continue;
        }

        // Try to grant locks to waiting requests in FIFO order
        for (auto& req : reqs) {
            if (req.granted) { continue; }

            // Check if this request conflicts with any currently granted requests
            bool conflict{false};
            for (const auto& other_req : reqs) {
                if (other_req.granted && conflicts(other_req.mode, req.mode)) {
                    conflict = true;
                    break;
                }
            }

            if (!conflict) {
                req.granted = true;
                if (req.wait_state) {
                    std::lock_guard wait_lock{req.wait_state->mutex};
                    req.wait_state->done = true;
                    req.wait_state->cv.notify_one();
                }
            } else {
                // FIFO lock scheduling: stop granting subsequent requests on conflict
                break;
            }
        }
    }
    tracked_txn_resources_.remove(id);
}

auto manager::is_wounded(id_t id) const noexcept -> bool {
    std::lock_guard lock{mutex_};
    return wounded_txns_.contains(id);
}

auto manager::acquire_lock(id_t id, resource_id_t res_id, mode_t mode) -> result<void> {
    std::unique_lock lock{mutex_};

    if (wounded_txns_.contains(id)) { return stdx::err{error_t::TXN_DEADLOCK_DETECTED}; }
    if (!bucket_table_.contains(res_id)) {
        if (bucket_table_.size() >= bucket_table_.capacity()) {
            return stdx::err{error_t::TXN_LOCK_ACQUISITION_FAILED};
        }
        bucket_table_.try_emplace(res_id);
    }
    auto& bucket{bucket_table_.get(res_id)};

    // Already holding the lock triggers reentrance
    for (auto it{bucket.requests.begin()}; it != bucket.requests.end(); ++it) {
        if (it->txn_id == id) {
            if (it->mode == mode || it->mode == mode_t::EXCLUSIVE) { return {}; }

            // Lock upgrade: remove the SHARED lock first, then proceed to request EXCLUSIVE.
            if (it->mode == mode_t::SHARED && mode == mode_t::EXCLUSIVE) {
                bucket.requests.erase(it);
                remove_tracked_resource(id, res_id);
                break;
            }
        }
    }

    // 4. Check compatibility with existing locks
    bool must_wait{false};
    for (const auto& req : bucket.requests) {
        if (req.granted && conflicts(req.mode, mode)) {
            if (id < req.txn_id) {
                wound(req.txn_id);
            } else {
                must_wait = true;
            }
        }
    }

    if (must_wait) {
        wait_state_t wait_state;

        if (bucket.requests.size() >= bucket.requests.capacity()) {
            return stdx::err{error_t::TXN_LOCK_ACQUISITION_FAILED};
        }

        bucket.requests.emplace_back(id, mode, false, wait_state);
        std::unique_lock wait_lock{wait_state.mutex};
        lock.unlock();
        wait_state.cv.wait(wait_lock, [&] { return wait_state.done || wait_state.aborted; });

        wait_lock.unlock();
        lock.lock();

        if (wait_state.aborted) {
            auto& reqs{bucket_table_.get(res_id).requests};
            for (auto it{reqs.begin()}; it != reqs.end(); ++it) {
                if (it->txn_id == id && !it->granted) {
                    reqs.erase(it);
                    break;
                }
            }
            if (reqs.empty()) { bucket_table_.remove(res_id); }
            return stdx::err{error_t::TXN_DEADLOCK_DETECTED};
        }

        if (auto res{add_tracked_resource(id, res_id)}; !res) {
            auto& reqs{bucket_table_.get(res_id).requests};
            for (auto it{reqs.begin()}; it != reqs.end(); ++it) {
                if (it->txn_id == id) {
                    reqs.erase(it);
                    break;
                }
            }
            if (reqs.empty()) { bucket_table_.remove(res_id); }
            return res;
        }
        return {};
    }

    if (bucket.requests.size() >= bucket.requests.capacity()) {
        return stdx::err{error_t::TXN_LOCK_ACQUISITION_FAILED};
    }
    bucket.requests.emplace_back(id, mode, true, stdx::none);

    auto res{add_tracked_resource(id, res_id)};
    if (!res) {
        bucket.requests.pop_back();
        if (bucket.requests.empty()) { bucket_table_.remove(res_id); }
    }
    return res;
}

auto manager::wound(id_t id) -> void {
    if (wounded_txns_.size() < wounded_txns_.capacity()) { wounded_txns_.emplace(id); }

    for (auto [_, bucket] : bucket_table_) {
        for (auto& req : bucket.requests) {
            if (req.txn_id == id && !req.granted && req.wait_state) {
                std::lock_guard wait_lock{req.wait_state->mutex};
                req.wait_state->aborted = true;
                req.wait_state->cv.notify_one();
            }
        }
    }
}

auto manager::add_tracked_resource(id_t id, resource_id_t res_id) -> result<void> {
    if (!tracked_txn_resources_.contains(id)) {
        if (tracked_txn_resources_.size() >= tracked_txn_resources_.capacity()) {
            return stdx::err{error_t::TXN_LOCK_ACQUISITION_FAILED};
        }
        tracked_txn_resources_.try_emplace(id);
    }

    auto& txn_res{tracked_txn_resources_.get(id)};
    if (txn_res.size() >= txn_res.capacity()) {
        return stdx::err{error_t::TXN_LOCK_ACQUISITION_FAILED};
    }
    txn_res.emplace_back(res_id);
    return {};
}

auto manager::remove_tracked_resource(id_t id, resource_id_t res_id) -> void {
    if (auto txn_res_opt{tracked_txn_resources_.get_opt(id)}) {
        auto& txn_res{*txn_res_opt};
        if (auto res_it{std::ranges::find(txn_res, res_id)}; res_it != txn_res.end()) {
            txn_res.erase(res_it);
        }
        if (txn_res.empty()) { tracked_txn_resources_.remove(id); }
    }
}

} // namespace cairn::txn::lock
