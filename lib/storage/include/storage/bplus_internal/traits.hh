#pragma once

#include <concepts>

#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

#include "storage/page.hh"

namespace cairn::storage::detail {

enum class node_kind : u8 {
    INTERNAL,
    LEAF,
};

using page_ptr_t       = gsl::not_null<page*>;
using const_page_ptr_t = gsl::not_null<const page*>;

template <typename T>
concept BPlusNodePayload = stdx::TriviallyCopyable<T> && stdx::DefaultConstructible<T>;

template <typename T, typename Key, typename Value>
concept BPlusLeafTrait = requires(page_ptr_t       p,
                                  const_page_ptr_t const_p,
                                  i32              idx,
                                  Key              key,
                                  Value            value,
                                  page_id_t        pid,
                                  Key&             sep) {
    { T::SLOTS } -> std::convertible_to<usize>;
    typename T::node_type;

    { T::init(p) } -> std::same_as<void>;
    { T::size(const_p) } -> std::same_as<i32>;
    { T::get_key(const_p, idx) } -> std::same_as<Key>;
    { T::get_value(const_p, idx) } -> std::same_as<Value>;
    { T::set_key(p, idx, key) } -> std::same_as<void>;
    { T::next(const_p) } -> std::same_as<stdx::option<page_id_t>>;
    { T::can_emplace(const_p, key, value) } -> std::same_as<bool>;
    { T::can_remove(const_p) } -> std::same_as<bool>;
    { T::emplace_at(p, idx, key, value) } -> std::same_as<void>;
    { T::remove_at(p, idx) } -> std::same_as<void>;
    { T::split(p, p, pid, idx, key, value) } -> std::same_as<Key>;
    { T::merge_into_left(p, p, sep) } -> std::same_as<void>;
    { T::borrow_from_left(p, p, sep) } -> std::same_as<void>;
    { T::borrow_from_right(p, p, sep) } -> std::same_as<void>;
};

template <typename T, typename Key, typename Comp>
concept BPlusInternalTrait = requires(
    page_ptr_t p, const_page_ptr_t const_p, i32 idx, Key key, page_id_t pid, Key& sep, Comp comp) {
    { T::SLOTS } -> std::convertible_to<usize>;
    typename T::node_type;

    { T::init(p) } -> std::same_as<void>;
    { T::init_root(p, key, pid, pid) } -> std::same_as<void>;
    { T::size(const_p) } -> std::same_as<i32>;
    { T::get_key(const_p, idx) } -> std::same_as<Key>;
    { T::set_key(p, idx, key) } -> std::same_as<void>;
    { T::get_child(const_p, idx) } -> std::same_as<page_id_t>;
    { T::can_emplace(const_p, key, pid) } -> std::same_as<bool>;
    { T::can_remove(const_p) } -> std::same_as<bool>;
    { T::emplace_at(p, idx, key, pid) } -> std::same_as<void>;
    { T::remove_at(p, idx) } -> std::same_as<void>;
    { T::split(p, p, key, pid, comp) } -> std::same_as<Key>;
    { T::merge_into_left(p, p, sep) } -> std::same_as<void>;
    { T::borrow_from_left(p, p, sep) } -> std::same_as<void>;
    { T::borrow_from_right(p, p, sep) } -> std::same_as<void>;
};

} // namespace cairn::storage::detail
