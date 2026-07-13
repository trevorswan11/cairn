#pragma once

#include <vector>

#include <gsl/span>
#include <stdx/iterator.hh>
#include <stdx/option.hh>
#include <stdx/utility.hh>

#include "support/diagnostic/error.hh"

namespace cairn {

class diagnostic_list {
  public:
    MAKE_ITERATOR(list_t, std::vector<diagnostic>, list_)

  public:
    constexpr explicit diagnostic_list(stdx::tribool in_terminal = stdx::none) noexcept
        : in_terminal_{in_terminal} {}
    ~diagnostic_list() = default;
    MAKE_MOVE_ONLY(diagnostic_list);

    template <typename... Args> auto emplace_back(Args&&... args) -> void {
        list_.emplace_back(std::forward<Args>(args)...);
    }

    [[nodiscard]] constexpr operator gsl::span<diagnostic>() noexcept { return list_; }
    [[nodiscard]] constexpr operator gsl::span<const diagnostic>() const noexcept { return list_; }

    // Creates a new list with the same terminal behavior
    [[nodiscard]] auto create_new() const -> diagnostic_list {
        return diagnostic_list{in_terminal_};
    }

    [[nodiscard]] auto get_terminal_status() const noexcept -> stdx::option<bool> {
        return in_terminal_;
    }

  private:
    list_t        list_;
    stdx::tribool in_terminal_;
};

} // namespace cairn
