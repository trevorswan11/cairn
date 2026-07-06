#include <cstddef>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>
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
                                            txn::INVALID_TXN_ID,
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

    auto read1{unwrap(undo_mgr.read_record(ptr1))};
    CHECK(read1.record.txn_id == txn::INVALID_TXN_ID);
    CHECK(read1.record.key == 100);
    CHECK(read1.record.op == undo::op_t::INSERT);
    CHECK(helpers::string_from_span(read1.payload) == payload1);

    auto read2{unwrap(undo_mgr.read_record(ptr2))};
    CHECK(read2.record.txn_id == txn::id_t{10});
    CHECK(read2.record.key == 100);
    CHECK(read2.record.op == undo::op_t::UPDATE);
    CHECK(read2.record.prev_undo_ptr == ptr1);
    CHECK(helpers::string_from_span(read2.payload) == payload2);
}

} // namespace cairn::tests
