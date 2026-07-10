#pragma once

#include <condition_variable>
#include <mutex>
#include <random>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace cairn::tests::helpers {

enum class thread_id_t : usize {};

namespace common_tids {

constexpr thread_id_t tid0{0};
constexpr thread_id_t tid1{1};

} // namespace common_tids

class deterministic_scheduler {
  public:
    deterministic_scheduler(u64 seed, usize num_threads);
    explicit deterministic_scheduler(std::vector<thread_id_t> sequence);

    [[nodiscard]] auto yield_to_schedule(thread_id_t thread_id) -> bool;
    auto               thread_exit(thread_id_t thread_id) -> void;

  private:
    std::mutex                             mutex_;
    std::condition_variable                cv_;
    std::mt19937                           rng_;
    stdx::option<thread_id_t>              turn_;
    stdx::option<thread_id_t>              running_thread_;
    stdx::option<std::vector<thread_id_t>> sequence_;
    usize                                  active_threads_{0};
    std::vector<bool>                      waiting_;
    usize                                  seq_idx_{0};
};

} // namespace cairn::tests::helpers
