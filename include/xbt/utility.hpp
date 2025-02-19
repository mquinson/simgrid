/* Copyright (c) 2016-2024. The SimGrid Team.
 * All rights reserved.                                                     */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#ifndef XBT_UTILITY_HPP
#define XBT_UTILITY_HPP

#include <array>
#include <functional>
#include <tuple>
#include <type_traits>
#include <xbt/base.h>

/** @brief Helper macro to declare enum class
 *
 * Declares an enum class EnumType, and a function "const char* to_c_str(EnumType)" to retrieve a C-string description
 * for each value.
 */
#define XBT_DECLARE_ENUM_CLASS(EnumType, ...)                                                                          \
  enum class EnumType;                                                                                                 \
  static constexpr char const* to_c_str(EnumType value)                                                                \
  {                                                                                                                    \
    constexpr std::array<const char*, _XBT_COUNT_ARGS(__VA_ARGS__)> names{{_XBT_STRINGIFY_ARGS(__VA_ARGS__)}};         \
    return ((int)value) >= 0 && ((unsigned long)value) < names.size() ? names.at(static_cast<int>(value))              \
                                                                      : "invalid value";                               \
  }                                                                                                                    \
  static constexpr bool is_valid_##EnumType(int raw_value)                                                             \
  {                                                                                                                    \
    return raw_value >= 0 && raw_value < _XBT_COUNT_ARGS(__VA_ARGS__);                                                 \
  }                                                                                                                    \
  enum class EnumType { __VA_ARGS__ } /* defined here to handle trailing semicolon */

namespace simgrid::xbt {

/** @brief Replacement for C++20's std::type_identity_t
 */
#if __cplusplus >= 201806L // __cpp_lib_type_identity
template <class T> using type_identity_t = typename std::type_identity_t<T>;
#else
template <class T> struct type_identity {
  using type = T;
};

template <class T> using type_identity_t = typename type_identity<T>::type;
#endif

/** @brief A hash which works with more stuff
 *
 *  It can hash pairs: the standard hash currently doesn't include this.
 */
template <class X> class hash : public std::hash<X> {
};

template <class X, class Y> class hash<std::pair<X, Y>> {
public:
  std::size_t operator()(std::pair<X, Y> const& x) const
  {
    hash<X> h1;
    hash<X> h2;
    return h1(x.first) ^ h2(x.second);
  }
};

/** @brief Comparator class for using with std::priority_queue or boost::heap.
 *
 * Compare two std::pair by their first element (of type double), and return true when the first is greater than the
 * second.  Useful to have priority queues with the smallest element on top.
 */
template <class Pair> class HeapComparator {
public:
  bool operator()(const Pair& a, const Pair& b) const { return a.first > b.first; }
};

/** @brief Erase an element given by reference from a boost::intrusive::list.
 */
template <class List, class Elem> inline void intrusive_erase(List& list, Elem& elem)
{
  list.erase(list.iterator_to(elem));
}

} // namespace simgrid::xbt
#endif
