#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <ios>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "storage/disk_manager.hh"
#include "storage/error.hh"
#include "storage/page.hh"
#include "testhelpers/common.hh"
#include "testhelpers/tempfile.hh"

namespace cairn::tests {

using namespace cairn::storage;

namespace {

template <usize N> auto seeded_pattern(std::array<std::byte, N>& buf, u8 seed) -> void {
    for (usize i{0}; auto& b : buf) { b = static_cast<std::byte>((i++ + seed) & 0xFF); }
}

} // namespace

TEST_CASE("Disk manager allocates dense page IDs") {
    helpers::tempfile file{"dm_alloc"};
    auto              dm{helpers::unwrap(disk_manager::open(file.path))};

    CHECK(dm->num_pages() == 0);
    CHECK(helpers::unwrap(dm->allocate_page()) == page_id_t{0});
    CHECK(helpers::unwrap(dm->allocate_page()) == page_id_t{1});
    CHECK(helpers::unwrap(dm->allocate_page()) == page_id_t{2});
    CHECK(dm->num_pages() == 3);
}

TEST_CASE("Disk manager round-trips page contents") {
    helpers::tempfile file{"dm_rw"};
    auto              dm{helpers::unwrap(disk_manager::open(file.path))};

    const auto                          pid{helpers::unwrap(dm->allocate_page())};
    std::array<std::byte, DB_PAGE_SIZE> buf_out{};
    seeded_pattern(buf_out, 42);
    REQUIRE(dm->write_page(pid, buf_out));

    std::array<std::byte, DB_PAGE_SIZE> buf_in{};
    REQUIRE(dm->read_page(pid, buf_in));
    CHECK(std::ranges::equal(buf_out, buf_in));
}

TEST_CASE("Disk manager rejects invalid page IDs") {
    helpers::tempfile file{"dm_invalid"};
    auto              dm{helpers::unwrap(disk_manager::open(file.path))};
    REQUIRE(dm->allocate_page());

    std::array<std::byte, DB_PAGE_SIZE> buf{};
    CHECK(helpers::unwrap_err(dm->read_page(page_id_t{5}, buf)) == error_t::INVALID_PAGE_ID);
    CHECK(helpers::unwrap_err(dm->read_page(page_id_t{-1}, buf)) == error_t::INVALID_PAGE_ID);
}

TEST_CASE("Disk manager persists across reopens") {
    helpers::tempfile                   file{"dm_persist"};
    std::array<std::byte, DB_PAGE_SIZE> buf_out{};
    seeded_pattern(buf_out, 67);
    page_id_t pid{INVALID_PAGE_ID};

    {
        auto dm{helpers::unwrap(disk_manager::open(file.path))};
        pid = helpers::unwrap(dm->allocate_page());
        REQUIRE(dm->allocate_page());
        REQUIRE(dm->write_page(pid, buf_out));
    }

    auto reopened{helpers::unwrap(disk_manager::open(file.path))};
    CHECK(reopened->num_pages() == 2);

    std::array<std::byte, DB_PAGE_SIZE> buf_in{};
    REQUIRE(reopened->read_page(pid, buf_in));
    CHECK(std::ranges::equal(buf_out, buf_in));
}

TEST_CASE("Disk manager short reads are detected") {
    helpers::tempfile file{"dm_short"};
    {
        std::fstream                            f{file.path, std::ios::out | std::ios::binary};
        std::array<std::byte, DB_PAGE_SIZE / 2> buf{};
        seeded_pattern(buf, 67);
        f.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    }

    std::array<std::byte, DB_PAGE_SIZE> buf{};
    auto                                dm{helpers::unwrap(disk_manager::open(file.path))};
    CHECK(helpers::unwrap_err(dm->read_page(page_id_t{0}, buf)) == error_t::SHORT_READ);
}

} // namespace cairn::tests
