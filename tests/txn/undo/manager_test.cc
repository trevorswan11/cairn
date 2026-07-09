#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/buffer_pool.hh"
#include "testhelpers/conversion.hh"
#include "testhelpers/tempfile.hh"
#include "testhelpers/unwrap.hh"
#include "txn/id.hh"
#include "txn/undo/manager.hh"

namespace cairn::tests {

using namespace cairn::txn;
using helpers::unwrap;

TEST_CASE("undo::manager read/write records") {
    helpers::tempfile file{"undo_mgr_test"};
    using pool_t = storage::buffer_pool<64>;
    auto                   pool{unwrap(pool_t::open(file.path))};
    undo::manager<i64, 64> undo_mgr{*pool};

    const std::string payload1{"first_version_data"};
    const std::string payload2{"second_version_data"};

    auto ptr1{unwrap(undo_mgr.append_record(txn::id_t{10},
                                            100,
                                            undo::op_t::INSERT,
                                            false,
                                            false,
                                            stdx::none,
                                            stdx::none,
                                            helpers::span_from_string(payload1)))};

    auto ptr2{unwrap(undo_mgr.append_record(txn::id_t{10},
                                            100,
                                            undo::op_t::UPDATE,
                                            false,
                                            false,
                                            txn::id_t{10},
                                            ptr1,
                                            helpers::span_from_string(payload2)))};
    CHECK(ptr1 != ptr2);

    std::vector<std::byte> rec_payload;
    const auto             rec1{unwrap(undo_mgr.read_record(ptr1, rec_payload))};
    CHECK_FALSE(rec1.txn_id);
    CHECK(rec1.key == 100);
    CHECK(rec1.op == undo::op_t::INSERT);
    CHECK(helpers::string_from_span(rec_payload) == payload1);

    const auto rec2{unwrap(undo_mgr.read_record(ptr2, rec_payload))};
    CHECK(unwrap(rec2.txn_id) == txn::id_t{10});
    CHECK(rec2.key == 100);
    CHECK(rec2.op == undo::op_t::UPDATE);
    CHECK(rec2.prev_undo_ptr == ptr1);
    CHECK(helpers::string_from_span(rec_payload) == payload2);
}

} // namespace cairn::tests
