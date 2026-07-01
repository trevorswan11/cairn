#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <random>
#include <utility>

#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/fixed/vector.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "storage/error.hh"
#include "storage/page.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::storage;

namespace {

using write_buf_t = gsl::span<std::byte, sizeof(i64)>;
[[nodiscard]] auto make_write_buf(std::byte* bytes) -> write_buf_t {
    return write_buf_t{bytes, write_buf_t::extent};
}

auto write_marker(std::byte* raw, i64 value) -> void {
    auto dest{make_write_buf(raw)};
    auto bytes = std::bit_cast<std::array<std::byte, sizeof(value)>>(value);
    std::copy(bytes.begin(), bytes.end(), dest.begin());
}

using read_buf_t = gsl::span<const std::byte, sizeof(i64)>;
[[nodiscard]] auto make_read_buf(const std::byte* bytes) -> read_buf_t {
    return read_buf_t{bytes, read_buf_t::extent};
}

[[nodiscard]] auto read_marker(const std::byte* raw) -> i64 {
    auto                               data{make_read_buf(raw)};
    std::array<std::byte, sizeof(i64)> bytes;
    std::copy(data.begin(), data.end(), bytes.begin());
    return std::bit_cast<i64>(bytes);
}

} // namespace

TEST_CASE("buffer pool round-trips a page through pin/unpin") {
    helpers::tempfile file{"bp_rw"};
    auto              bp{helpers::unwrap(buffer_pool<8>::open(file.path))};

    page_id_t pid{INVALID_PAGE_ID};
    {
        auto pg{helpers::unwrap(bp->new_page())};
        pid = pg->page_id();
        write_marker(pg->data(), 0xbeef);
        CHECK(bp->pin_count(pid) == 1);
        REQUIRE(bp->unpin_page(pid, true));
        CHECK(bp->pin_count(pid) == 0);
    }

    auto again{helpers::unwrap(bp->fetch_page(pid))};
    CHECK(read_marker(again->data()) == 0xbeef);
    REQUIRE(bp->unpin_page(pid, false));
}

TEST_CASE("buffer pool reports exhaustion when every frame is pinned") {
    helpers::tempfile file{"bp_exhaust"};
    using pool_t = buffer_pool<3>;
    auto bp{helpers::unwrap(pool_t::open(file.path))};

    stdx::fixed::vector<page_id_t, pool_t::pool_size> ids;
    for (usize i{0}; i < ids.capacity(); ++i) {
        auto pg{helpers::unwrap(bp->new_page())};
        ids.emplace_back(pg->page_id());
    }

    // All frames full must error
    REQUIRE(helpers::unwrap_err(bp->new_page()) == storage::error_t::POOL_EXHAUSTED);
    REQUIRE(bp->unpin_page(ids.front(), false));
    CHECK(bp->new_page());
}

TEST_CASE("buffer pool evicts and reloads cold pages") {
    helpers::tempfile file{"bp_evict"};
    auto              bp{helpers::unwrap(buffer_pool<2>::open(file.path))};

    stdx::fixed::vector<page_id_t, 3> ids;
    for (usize i{0}; i < ids.capacity(); ++i) {
        auto pg{helpers::unwrap(bp->new_page())};
        write_marker(pg->data(), 100 + static_cast<i64>(i));

        // Unpin the page since the pool can only fit 2
        REQUIRE(bp->unpin_page(pg->page_id(), true));
        ids.emplace_back(pg->page_id());
    }

    auto reloaded{helpers::unwrap(bp->fetch_page(ids.front()))};
    CHECK(read_marker(reloaded->data()) == 100);
    REQUIRE(bp->unpin_page(ids.front(), false));
}

TEST_CASE("buffer pool guards latch and unlatch") {
    helpers::tempfile file{"bp_guard"};
    auto              bp{helpers::unwrap(buffer_pool<8>::open(file.path))};

    page_id_t pid{INVALID_PAGE_ID};
    {
        auto [id, guard]{helpers::unwrap(bp->new_write())};
        write_marker(guard.as<std::byte>(), 1'234);
        guard.mark_dirty();
        CHECK(bp->pin_count(id) == 1);
        pid = id;
    }
    CHECK(bp->pin_count(pid) == 0);

    {
        auto guard{helpers::unwrap(bp->fetch_read(pid))};
        CHECK(read_marker(guard.as<std::byte>()) == 1'234);
    }
    CHECK(bp->pin_count(pid) == 0);
}

TEST_CASE("buffer pool persists pages across reopen") {
    helpers::tempfile file{"bp_persist"};
    using pool_t = buffer_pool<4>;

    stdx::fixed::vector<page_id_t, 6> ids;
    {
        auto bp{helpers::unwrap(pool_t::open(file.path))};
        for (usize i{0}; i < ids.capacity(); ++i) {
            auto pg{helpers::unwrap(bp->new_page())};
            write_marker(pg->data(), 500 + static_cast<i64>(i));
            REQUIRE(bp->unpin_page(pg->page_id(), true));
            ids.emplace_back(pg->page_id());
        }
        REQUIRE(bp->flush());
    }

    auto bp{helpers::unwrap(pool_t::open(file.path))};
    for (i64 i{0}; const auto pid : ids) {
        auto pg{helpers::unwrap(bp->fetch_page(pid))};
        CHECK(read_marker(pg->data()) == 500 + i++);
        REQUIRE(bp->unpin_page(pid, false));
    }
}

TEST_CASE("buffer pool stays correct under heavy churn") {
    helpers::tempfile file{"bp_churn"};
    using pool_t = buffer_pool<8>;
    auto bp{helpers::unwrap(pool_t::open(file.path))};

    stdx::fixed::vector<page_id_t, 6'000> ids;
    for (usize i{0}; i < ids.capacity(); ++i) {
        auto pg{helpers::unwrap(bp->new_page())};
        ids.emplace_back(pg->page_id());
        write_marker(pg->data(), std::to_underlying(ids[i]));
        REQUIRE(bp->unpin_page(ids[i], true));
    }

    // Following churn the id should still be resolved
    std::mt19937_64 rng{Catch::getSeed()};
    std::ranges::shuffle(ids, rng);
    for (const auto pid : ids) {
        auto pg{helpers::unwrap(bp->fetch_page(pid))};
        CHECK(read_marker(pg->data()) == std::to_underlying(pid));
        REQUIRE(bp->unpin_page(pid, false));
    }
}

} // namespace cairn::tests
