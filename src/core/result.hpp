// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CORE_RESULT_HPP
#define LOADFORGE_CORE_RESULT_HPP

#include <type_traits>
#include <utility>
#include <variant>

namespace loadforge::core {

/// Success carrying no information, for Result<Ok, E>: "it worked", with
/// nothing to hand back. std::monostate would serve, but a named type says why
/// it is there, and Result requires its two alternatives to be distinct types.
///
/// Not called Unit: duration.cpp and byte_size.cpp already use that name for a
/// unit suffix, and the older, more specific meaning keeps it.
struct Ok {
  [[nodiscard]] friend constexpr bool operator==(Ok /*lhs*/, Ok /*rhs*/) noexcept { return true; }
};

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
  // Both constructors take by value and MOVE into the variant, so each is
  // exactly as noexcept as that move is -- and saying so is not decoration.
  //
  // Without it, every `return SomeErrorStruct{...};` in the project compiles to
  // a call the compiler must assume can throw, which forces a landing pad that
  // destroys the just-built temporary. That cleanup block contains a branch (the
  // small-string check inside ~basic_string) which NO TEST CAN EVER TAKE, and
  // gcovr's --exclude-throw-branches does not remove it: the edge lives inside
  // the cleanup block rather than being labelled a throw edge itself. The
  // result was an uncoverable branch appearing in whichever module happened to
  // build an error type with two allocating members -- a coverage exclusion
  // demanded by nothing but a missing specifier.
  //
  // Declaring the truth here is rung 2 of the exclusion ladder (make it
  // reachable) reached from the other side: remove the unreachable edge rather
  // than excuse it. The condition is not a guess -- std::variant's in-place
  // constructor is itself noexcept exactly when the alternative's constructor
  // is -- so a type whose move can throw still gets a throwing Result and the
  // landing pad it genuinely needs.
  constexpr Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
      : store_(std::in_place_index<0>, std::move(value)) {}
  constexpr Result(E error) noexcept(std::is_nothrow_move_constructible_v<E>)
      : store_(std::in_place_index<1>, std::move(error)) {}

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
