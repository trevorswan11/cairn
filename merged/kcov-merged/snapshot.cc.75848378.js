var data = {lines:[
{"lineNum":"    1","line":"#include \"txn/snapshot.hh\""},
{"lineNum":"    2","line":""},
{"lineNum":"    3","line":"#include <algorithm>"},
{"lineNum":"    4","line":""},
{"lineNum":"    5","line":"#include \"txn/id.hh\""},
{"lineNum":"    6","line":""},
{"lineNum":"    7","line":"namespace cairn::txn {"},
{"lineNum":"    8","line":""},
{"lineNum":"    9","line":"auto snapshot_t::is_active(id_t id) const noexcept -> bool {","class":"lineCov","hits":"1","order":"1936",},
{"lineNum":"   10","line":"    if (id < xmin || id >= xmax) { return false; }","class":"lineCov","hits":"1","order":"1935",},
{"lineNum":"   11","line":"    return std::ranges::binary_search(active_txns, id);","class":"lineCov","hits":"1","order":"1934",},
{"lineNum":"   12","line":"}","class":"lineCov","hits":"1","order":"1933",},
{"lineNum":"   13","line":""},
{"lineNum":"   14","line":"} // namespace cairn::txn"},
]};
var percent_low = 25;var percent_high = 75;
var header = { "command" : "", "date" : "2026-07-24 01:31:31", "instrumented" : 4, "covered" : 4,};
var merged_data = [];
