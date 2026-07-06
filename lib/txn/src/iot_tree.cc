#include "txn/iot_tree.hh"

#include <cstddef>
#include <cstring>

#include <gsl/span>

namespace cairn::txn {

auto read_version_header(gsl::span<const std::byte> val) -> tuple_version_header_t {
    tuple_version_header_t header;
    std::memcpy(&header, val.data(), sizeof(tuple_version_header_t));
    return header;
}

auto read_payload(gsl::span<const std::byte> val) -> gsl::span<const std::byte> {
    return val.subspan(sizeof(tuple_version_header_t));
}

} // namespace cairn::txn
