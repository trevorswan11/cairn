#pragma once

#include <type_traits>
#include <vector>

#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "sql/detail/node.hh"
#include "support/diagnostic/location.hh"

namespace cairn {

namespace sql::detail {

template <typename Data> class ast_t {
  public:
    ast_t()  = default;
    ~ast_t() = default;
    MAKE_MOVE_ONLY(ast_t);

    auto clear() noexcept -> void {
        pool_.clear();
        roots_.clear();
        locations_.clear();
    }

    template <typename T, typename... Args>
    auto add_node(const Locateable auto& current_loc, Args&&... args) -> node_id_t {
        const u64 index{static_cast<u64>(pool_.size())};
        pool_.emplace_back(std::in_place_type<T>, std::forward<Args>(args)...);

        using loc_t = std::remove_cvref_t<decltype(current_loc)>;
        const auto loc{source_info<loc_t>::get(current_loc)};
        locations_.emplace_back(loc.line, loc.column);
        return node_id_t{node_kind_of<T>(), index};
    }

    [[nodiscard]] auto get_location(node_id_t id) const noexcept -> location {
        ASSERT(id.is_valid() && id.index() < locations_.size());
        return locations_[id.index()];
    }

    auto add_root(node_id_t id) noexcept -> void { roots_.emplace_back(id); }

    [[nodiscard]] auto roots() const noexcept -> gsl::span<const node_id_t> { return roots_; }

    [[nodiscard]] auto operator[](node_id_t id) const noexcept -> const Data& {
        ASSERT(id.is_valid() && id.index() < pool_.size());
        return pool_[id.index()];
    }

    [[nodiscard]] auto operator[](node_id_t id) noexcept -> Data& {
        ASSERT(id.is_valid() && id.index() < pool_.size());
        return pool_[id.index()];
    }

    template <typename T>
    [[nodiscard]] auto get_as_opt(node_id_t id) const noexcept -> stdx::option<const T&> {
        return operator[](id).template as_opt<T>();
    }

  private:
    std::vector<Data>      pool_;
    std::vector<node_id_t> roots_;
    std::vector<location>  locations_;
};

} // namespace sql::detail

} // namespace cairn
