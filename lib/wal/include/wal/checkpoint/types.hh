#pragma once

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "storage/page.hh"
#include "txn/id.hh"
#include "wal/log/seq_num.hh"

namespace cairn::wal::checkpoint {

// An entry in the dirty page table
struct dpt_entry {
    storage::page_id_t page_id;
    log::seq_num       rec_lsn;
};

// An entry in the active transaction table
struct att_entry {
    enum class state_t : u8 {
        ACTIVE,
        COMMITTED,
        ABORTED,
    };

    txn::id_t                  txn_id;
    state_t                    state;
    stdx::option<log::seq_num> last_lsn;
};

} // namespace cairn::wal::checkpoint
