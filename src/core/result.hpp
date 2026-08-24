// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CORE_RESULT_HPP
#define LOADFORGE_CORE_RESULT_HPP

#include <utility>
#include <variant>

namespace loadforge::core {

/// A value or the reason one could not be produced.
///
/// std::expected is C++23; the reference toolchain is GCC 13 / C++20
/// (docs/platforms.md), so the project carries this small equivalent rather
/// than raising the toolchain floor for one type.
///
/// Misuse is *defined*: value() on an error throws std::bad_variant_access
/// rather than dereferencing a null pointer. An assert would vanish under
/// NDEBUG and leave undefined behaviour behind, which the project refuses to
/// rely on anywhere (design draft §40) -- least of all in a tool whose job is
/// distinguishing real hardware faults from its own bugs.
template <typename T, typename E>
class Result {
 public:
  constexpr Result(T value) : store_(std::in_place_index<0>, std::move(value)) {}
  constexpr Result(E error) : store_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] constexpr bool has_value() const noexcept { return store_.index() == 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

  /// The value. Throws std::bad_variant_access if this holds an error.
  [[nodiscard]] constexpr const T& value() const { return std::get<0>(store_); }

  /// The error. Throws std::bad_variant_access if this holds a value.
  [[nodiscard]] constexpr const E& error() const { return std::get<1>(store_); }

  /// The value if present, otherwise the supplied fallback. Never throws on
  /// the absent path: has_value() has already selected the alternative.
  [[nodiscard]] constexpr T value_or(T fallback) const {
    return has_value() ? *std::get_if<0>(&store_) : std::move(fallback);
  }

 private:
  std::variant<T, E> store_;
};

}  // namespace loadforge::core

#endif
