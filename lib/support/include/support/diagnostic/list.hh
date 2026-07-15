#pragma once

#include <vector>

#include <gsl/span>
#include <stdx/iterator.hh>
#include <stdx/utility.hh>
#include <stdx/types.hh>

#include "support/diagnostic/error.hh"

namespace cairn {

class diagnostic_list {
  public:
    MAKE_ITERATOR(list_t, std::vector<diagnostic>, list_)

  public:
    diagnostic_list() = default;
    ~diagnostic_list() = default;
    MAKE_MOVE_ONLY(diagnostic_list);

    auto emplace_back(error err, usize line, usize column) -> void {
        list_.emplace_back(err, line, column);
    }

    [[nodiscard]] constexpr operator gsl::span<diagnostic>() noexcept { return list_; }
    [[nodiscard]] constexpr operator gsl::span<const diagnostic>() const noexcept { return list_; }

  private:
    list_t list_;
};

} // namespace cairn
