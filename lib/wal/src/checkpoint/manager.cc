#include "wal/checkpoint/manager.hh"

#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <ios>
#include <system_error>
#include <utility>

#include <stdx/result.hh>

#include "support/diagnostic/error.hh"
#include "wal/log/seq_num.hh"

namespace cairn::wal::checkpoint {

manager::manager(std::filesystem::path control_path) noexcept
    : control_path_{std::move(control_path)} {}

auto manager::read_latest_checkpoint_lsn() -> result<log::seq_num> {
    if (!std::filesystem::exists(control_path_)) {
        return stdx::err{error::WAL_CONTROL_PATH_NOT_FOUND};
    }

    std::ifstream in{control_path_, std::ios::in | std::ios::binary};
    if (!in.is_open()) { return stdx::err{error::IO_ERROR}; }
    std::array<char, sizeof(log::seq_num)> checkpoint_lsn;
    in.read(checkpoint_lsn.data(), checkpoint_lsn.size());
    if (in.fail()) { return stdx::err{error::IO_ERROR}; }
    return std::bit_cast<log::seq_num>(checkpoint_lsn);
}

auto manager::persist_lsn(log::seq_num lsn) -> result<void> {
    auto temp_path{control_path_};
    temp_path.replace_extension(".tmp");

    {
        std::ofstream out{temp_path, std::ios::out | std::ios::binary | std::ios::trunc};
        if (!out.is_open()) { return stdx::err{error::IO_ERROR}; }
        out.write(reinterpret_cast<const char*>(&lsn), sizeof(lsn));
    }

    std::error_code ec;
    std::filesystem::rename(temp_path, control_path_, ec);
    if (ec) { return stdx::err{error::IO_ERROR}; }
    return {};
}

} // namespace cairn::wal::checkpoint
