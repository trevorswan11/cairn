var data = {lines:[
{"lineNum":"    1","line":"#pragma once"},
{"lineNum":"    2","line":""},
{"lineNum":"    3","line":"#include <functional>"},
{"lineNum":"    4","line":""},
{"lineNum":"    5","line":"#include <stdx/option.hh>"},
{"lineNum":"    6","line":""},
{"lineNum":"    7","line":"#include \"support/diagnostic/error.hh\""},
{"lineNum":"    8","line":"#include \"txn/id.hh\""},
{"lineNum":"    9","line":""},
{"lineNum":"   10","line":"namespace cairn::txn {"},
{"lineNum":"   11","line":""},
{"lineNum":"   12","line":"class read_set_t {","class":"lineCov","hits":"1","order":"2380",},
{"lineNum":"   13","line":"  public:"},
{"lineNum":"   14","line":"    using get_commit_ts_t = std::function<stdx::option<timestamp_t>(id_t)>;"},
{"lineNum":"   15","line":""},
{"lineNum":"   16","line":"  public:"},
{"lineNum":"   17","line":"    virtual ~read_set_t() = default;","class":"lineCov","hits":"1","order":"2379",},
{"lineNum":"   18","line":"    [[nodiscard]] virtual auto"},
{"lineNum":"   19","line":"    check_conflict(id_t reader_id, timestamp_t read_ts, const get_commit_ts_t& get_commit_ts) const"},
{"lineNum":"   20","line":"        -> result<bool> = 0;"},
{"lineNum":"   21","line":"};"},
{"lineNum":"   22","line":""},
{"lineNum":"   23","line":"} // namespace cairn::txn"},
]};
var percent_low = 25;var percent_high = 75;
var header = { "command" : "", "date" : "2026-07-24 01:31:31", "instrumented" : 2, "covered" : 2,};
var merged_data = [];
