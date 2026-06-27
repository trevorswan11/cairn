#pragma once

#include <array>
#include <utility>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

namespace cairn::storage {

// Index of a physical frame inside the buffer pool.
enum class frame_id_t : i32 {};

constexpr frame_id_t INVALID_FRAME_ID{-1};

} // namespace cairn::storage

namespace stdx {

template <> struct nullable<cairn::storage::frame_id_t> {
    using fid_t = cairn::storage::frame_id_t;
    [[nodiscard]] static constexpr auto invalid() noexcept -> fid_t {
        return cairn::storage::INVALID_FRAME_ID;
    }

    [[nodiscard]] static constexpr auto is_valid(const fid_t& id) noexcept -> bool {
        return id != invalid();
    }
};

} // namespace stdx

namespace cairn::storage {

// Frames with the largest backward k-distance are evicted first
//
// https://www.cs.cmu.edu/~natassa/courses/15-721/papers/p297-o_neil.pdf
template <usize Capacity, usize K = 2>
    requires(Capacity > 0 && K > 0)
class frame_replacer {
  public:
    // Uses the current logical time to record frame access
    auto access(frame_id_t fid) noexcept -> void {
        auto& node{node_at(fid)};
        node.present = true;
        node.push(now_++);
    }

    // Forgets a frame entirely
    auto remove(frame_id_t fid) noexcept -> void {
        auto& node{node_at(fid)};
        if (!node.present) { return; }
        if (node.evictable) { evictable_ -= 1; }
        node = node_t{};
    }

    auto set_evictable(frame_id_t fid, bool evictable) noexcept -> void {
        auto& node{node_at(fid)};
        if (!node.present) { return; }
        if (node.evictable != evictable) {
            node.evictable = evictable;
            if (evictable) {
                evictable_ += 1;
            } else {
                evictable_ -= 1;
            }
        }
    }

    // Picks a frame and evicts it per LRU-K
    [[nodiscard]] auto evict() noexcept -> stdx::option<frame_id_t> {
        stdx::option<frame_id_t> inf_victim;
        u64                      inf_oldest{static_cast<u64>(-1)};
        stdx::option<frame_id_t> fin_victim;
        u64                      fin_oldest{inf_oldest};

        for (usize i{0}; i < Capacity; ++i) {
            const auto& node{nodes_[i]};
            if (!node.present || !node.evictable) { continue; }

            const auto oldest{node.oldest()};
            if (node.count < K && oldest < inf_oldest) {
                inf_oldest = oldest;
                inf_victim.emplace(static_cast<frame_id_t>(i));
            } else if (oldest < fin_oldest) {
                fin_oldest = oldest;
                fin_victim.emplace(static_cast<frame_id_t>(i));
            }
        }

        const stdx::option<frame_id_t> victim{inf_victim ? inf_victim : fin_victim};
        return victim.transform([this](frame_id_t fid) {
            remove(fid);
            return fid;
        });
    }

    [[nodiscard]] auto size() const noexcept -> usize { return evictable_; }

  private:
    // Per-frame access history represented as a ring of recent K timestamps
    struct node_t {
        std::array<u64, K> hist{};
        usize              start{0};
        usize              count{0};
        bool               evictable{false};
        bool               present{false};

        auto push(u64 ts) noexcept -> void {
            if (count < K) {
                hist[(start + count) % K] = ts;
                count += 1;
            } else {
                hist[start] = ts;
                start       = (start + 1) % K;
            }
        }

        [[nodiscard]] auto oldest() const noexcept -> u64 { return count == 0 ? 0 : hist[start]; }
    };

  private:
    [[nodiscard]] auto node_at(this auto&& self, frame_id_t fid) noexcept -> auto& {
        const auto idx{std::to_underlying(fid)};
        ASSERT(idx >= 0 && static_cast<usize>(idx) < Capacity, "frame id out of range");
        return self.nodes_[static_cast<usize>(idx)];
    }

  private:
    std::array<node_t, Capacity> nodes_;
    u64                          now_{1};
    usize                        evictable_{0};
};

} // namespace cairn::storage
