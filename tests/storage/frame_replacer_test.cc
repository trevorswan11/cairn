#include <catch2/catch_test_macros.hpp>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "storage/frame_replacer.hh"
#include "testhelpers/unwrap.hh"

namespace cairn::tests {

using namespace cairn::storage;

TEST_CASE("replacer evicts LRU among under-K frames") {
    frame_replacer<8> replacer;
    for (i32 f{0}; f < 3; ++f) {
        replacer.access(frame_id_t{f});
        replacer.set_evictable(frame_id_t{f}, true);
    }
    CHECK(replacer.size() == 3);

    // Insert time should be used since all have single access
    CHECK(helpers::unwrap(replacer.evict()) == frame_id_t{0});
    CHECK(helpers::unwrap(replacer.evict()) == frame_id_t{1});
    CHECK(helpers::unwrap(replacer.evict()) == frame_id_t{2});
    CHECK(replacer.size() == 0);
    CHECK_FALSE(replacer.evict());
}

TEST_CASE("replacer prefers under-K frames") {
    frame_replacer<8> replacer;

    replacer.access(frame_id_t{0});
    replacer.access(frame_id_t{0});
    replacer.access(frame_id_t{1});
    replacer.set_evictable(frame_id_t{0}, true);
    replacer.set_evictable(frame_id_t{1}, true);

    CHECK(helpers::unwrap(replacer.evict()) == frame_id_t{1});
    CHECK(helpers::unwrap(replacer.evict()) == frame_id_t{0});
}

TEST_CASE("replacer orders full-history by k-distance") {
    frame_replacer<8> replacer;

    replacer.access(frame_id_t{0});
    replacer.access(frame_id_t{1});
    replacer.access(frame_id_t{0});
    replacer.access(frame_id_t{1});

    replacer.set_evictable(frame_id_t{0}, true);
    replacer.set_evictable(frame_id_t{1}, true);

    CHECK(helpers::unwrap(replacer.evict()) == frame_id_t{0});
    CHECK(helpers::unwrap(replacer.evict()) == frame_id_t{1});
}

TEST_CASE("replacer respects evictability and removal") {
    frame_replacer<8> replacer;
    for (i32 f{0}; f < 4; ++f) {
        replacer.access(frame_id_t{f});
        replacer.set_evictable(frame_id_t{f}, true);
    }
    replacer.set_evictable(frame_id_t{1}, false);

    CHECK(replacer.size() == 3);
    replacer.remove(frame_id_t{2});
    CHECK(replacer.size() == 2);

    // 1 should never be evicted and 2 should've been dropped
    CHECK(helpers::unwrap(replacer.evict()) == frame_id_t{0});
    CHECK(helpers::unwrap(replacer.evict()) == frame_id_t{3});
    CHECK_FALSE(replacer.evict());
}

} // namespace cairn::tests
