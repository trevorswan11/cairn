#include <mutex>
#include <shared_mutex>

#include <catch2/catch_test_macros.hpp>

#include "storage/rw_latch.hh"

namespace cairn::tests {

TEST_CASE("rw_latch support manual lock cycles") {
    storage::rw_latch latch;

    SECTION("Exclusive cycle") {
        latch.lock();
        latch.unlock();
    }

    SECTION("Shared cycle") {
        latch.lock_shared();
        latch.unlock_shared();
    }

    SECTION("Two concurrent shared holders on one thread") {
        latch.lock_shared();
        latch.lock_shared();
        latch.unlock_shared();
        latch.unlock_shared();
    }
}

TEST_CASE("rw_latch honors try_lock semantics") {
    storage::rw_latch latch;

    REQUIRE(latch.try_lock());
    latch.unlock();

    REQUIRE(latch.try_lock_shared());
    latch.unlock_shared();
}

TEST_CASE("rw_latch supports use by std wrappers") {
    storage::rw_latch latch;

    {
        std::unique_lock exclusive{latch};
        CHECK(exclusive.owns_lock());
    }
    {
        std::shared_lock shared{latch};
        CHECK(shared.owns_lock());
    }
}

} // namespace cairn::tests
