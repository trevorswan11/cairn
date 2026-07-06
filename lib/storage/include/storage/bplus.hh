#pragma once

#include <cstddef>
#include <functional>

#include <gsl/span>
#include <stdx/types.hh>

#include "storage/bplus_internal/base.hh"
#include "storage/bplus_internal/default_impl.hh"
#include "storage/bplus_internal/iot_impl.hh"
#include "storage/bplus_internal/traits.hh"

namespace cairn::storage {

// A general purpose b+ tree supporting trivially copyable and constructible types
template <detail::BPlusNodePayload Key,
          detail::BPlusNodePayload Value,
          usize                    PoolSize,
          typename Compare                             = std::less<Key>,
          detail::BPlusLeafTrait<Key, Value> LeafTrait = detail::default_leaf_trait<Key, Value>,
          detail::BPlusInternalTrait<Key, Compare> InternalTrait =
              detail::default_internal_trait<Key>>
using bplus_tree = detail::bplus_base_t<Key, Value, PoolSize, Compare, LeafTrait, InternalTrait>;

// An index-organized table for efficient tuple storage inside a b+ tree
template <detail::BPlusNodePayload Key,
          usize                    MaxTupleSize,
          usize                    PoolSize,
          typename Compare = std::less<Key>>
using iot_tree = detail::bplus_base_t<Key,
                                      gsl::span<const std::byte>,
                                      PoolSize,
                                      Compare,
                                      detail::iot_leaf_trait<Key, MaxTupleSize>,
                                      detail::default_internal_trait<Key>>;

} // namespace cairn::storage
