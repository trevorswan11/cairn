#pragma once

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/page.hh"
#include "txn/id.hh"
#include "wal/log_sequence_number.hh"

namespace cairn::wal {

struct checkpoint_dpt_entry {
    storage::page_id_t page_id;
    lsn_t              rec_lsn;
};

enum class att_state_t : u8 {
    ACTIVE,
    COMMITTED,
    ABORTED,
};

struct checkpoint_att_entry {
    txn::id_t                txn_id;
    att_state_t              state;
    stdx::option<wal::lsn_t> last_lsn;
};

} // namespace cairn::wal
