var data = {lines:[
{"lineNum":"    1","line":"#include \"txn/iot_tree.hh\""},
{"lineNum":"    2","line":""},
{"lineNum":"    3","line":"#include <cstddef>"},
{"lineNum":"    4","line":"#include <cstring>"},
{"lineNum":"    5","line":""},
{"lineNum":"    6","line":"#include <gsl/span>"},
{"lineNum":"    7","line":""},
{"lineNum":"    8","line":"namespace cairn::txn {"},
{"lineNum":"    9","line":""},
{"lineNum":"   10","line":"auto read_version_header(gsl::span<const std::byte> val) -> tuple_version_header_t {","class":"lineCov","hits":"1","order":"1949",},
{"lineNum":"   11","line":"    tuple_version_header_t header;","class":"lineCov","hits":"1","order":"1948",},
{"lineNum":"   12","line":"    std::memcpy(&header, val.data(), sizeof(tuple_version_header_t));","class":"lineCov","hits":"1","order":"1946",},
{"lineNum":"   13","line":"    return header;","class":"lineCov","hits":"1","order":"1945",},
{"lineNum":"   14","line":"}"},
{"lineNum":"   15","line":""},
{"lineNum":"   16","line":"auto read_payload(gsl::span<const std::byte> val) -> gsl::span<const std::byte> {","class":"lineCov","hits":"1","order":"1947",},
{"lineNum":"   17","line":"    return val.subspan(sizeof(tuple_version_header_t));","class":"lineCov","hits":"1","order":"1944",},
{"lineNum":"   18","line":"}"},
{"lineNum":"   19","line":""},
{"lineNum":"   20","line":"} // namespace cairn::txn"},
]};
var percent_low = 25;var percent_high = 75;
var header = { "command" : "", "date" : "2026-07-24 01:31:31", "instrumented" : 6, "covered" : 6,};
var merged_data = [];
