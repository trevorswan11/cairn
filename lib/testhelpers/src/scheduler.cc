#include "testhelpers/scheduler.hh"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

#include <stdx/types.hh>

namespace cairn::tests::helpers {

deterministic_scheduler::deterministic_scheduler(u64 seed, usize num_threads)
    : rng_{static_cast<std::mt19937::result_type>(seed)}, active_threads_{num_threads},
      waiting_(num_threads, false) {}

deterministic_scheduler::deterministic_scheduler(std::vector<thread_id_t> sequence)
    : sequence_{std::move(sequence)},
      active_threads_{std::to_underlying(std::ranges::max(*sequence_)) + 1},
      waiting_(active_threads_, false) {}

auto deterministic_scheduler::yield_to_schedule(thread_id_t thread_id) -> bool {
    std::unique_lock lock{mutex_};
    const auto       u_thread_id{static_cast<usize>(thread_id)};
    waiting_[u_thread_id] = true;

    const auto timeout = std::chrono::seconds(5);

    while (true) {
        if (sequence_) {
            if (seq_idx_ < sequence_->size() && !turn_ &&
                (!running_thread_ || waiting_[static_cast<usize>(*running_thread_)])) {
                auto next_tid{sequence_->at(seq_idx_)};
                if (waiting_[static_cast<usize>(next_tid)]) {
                    turn_.emplace(next_tid);
                    running_thread_.emplace(next_tid);
                    seq_idx_++;
                    cv_.notify_all();
                }
            }
        } else {
            const auto wait_count{
                static_cast<usize>(std::ranges::count_if(waiting_, [](bool v) { return v; }))};

            if (wait_count == active_threads_) {
                std::vector<thread_id_t> candidates;
                for (usize i{0}; i < waiting_.size(); ++i) {
                    if (waiting_[i]) { candidates.emplace_back(thread_id_t{i}); }
                }

                if (!candidates.empty()) { turn_.emplace(candidates[rng_() % candidates.size()]); }
                cv_.notify_all();
            }
        }

        if (turn_ == thread_id || active_threads_ == 0) {
            waiting_[u_thread_id] = false;
            turn_.reset();
            return true;
        }

        if (cv_.wait_for(lock, timeout) == std::cv_status::timeout) {
            waiting_[u_thread_id] = false;
            return false;
        }
    }
}

auto deterministic_scheduler::thread_exit(thread_id_t thread_id) -> void {
    std::unique_lock lock{mutex_};
    active_threads_--;
    waiting_[static_cast<usize>(thread_id)] = false;
    if (turn_ == thread_id) { turn_.reset(); }
    if (running_thread_ == thread_id) { running_thread_.reset(); }
    cv_.notify_all();
}

} // namespace cairn::tests::helpers
