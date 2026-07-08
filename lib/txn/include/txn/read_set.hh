#pragma once

#include <functional>

#include <stdx/option.hh>

#include "support/error.hh"
#include "txn/id.hh"

namespace cairn::txn {

class read_set_t {
  public:
    using get_commit_ts_t = std::function<stdx::option<timestamp_t>(id_t)>;

  public:
    virtual ~read_set_t() = default;
    [[nodiscard]] virtual auto
    check_conflict(id_t reader_id, timestamp_t read_ts, const get_commit_ts_t& get_commit_ts) const
        -> result<bool> = 0;
};

} // namespace cairn::txn
