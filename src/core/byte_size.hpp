// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CORE_BYTE_SIZE_HPP
#define LOADFORGE_CORE_BYTE_SIZE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "core/parse_error.hpp"
#include "core/result.hpp"

namespace loadforge::core {

/// A configured memory footprint, which the design draft (§37) allows to be
/// written either absolutely ("8GiB") or as a share of what is available
/// ("70%").
///
/// This type deliberately does NOT resolve a percentage. Doing so requires
/// MemAvailable and the cgroup limit, whichever is lower (PLAN.md F7), which is
/// platform knowledge and belongs behind the platform boundary. Core parses;
/// something that can see the machine resolves.
class ByteSize {
 public:
  enum class Kind { Absolute, Fraction };

  static constexpr ByteSize bytes(std::uint64_t count) noexcept {
    return ByteSize{Kind::Absolute, count};
  }
  static constexpr ByteSize percent(std::uint64_t whole) noexcept {
    return ByteSize{Kind::Fraction, whole};
  }

  [[nodiscard]] constexpr Kind kind() const noexcept { return kind_; }
  [[nodiscard]] constexpr bool is_absolute() const noexcept { return kind_ == Kind::Absolute; }

  /// Precondition: is_absolute().
  [[nodiscard]] constexpr std::uint64_t byte_count() const noexcept { return value_; }

  /// Precondition: !is_absolute(). In whole percent, 1..100.
  [[nodiscard]] constexpr std::uint64_t percent_of_available() const noexcept { return value_; }

  friend constexpr bool operator==(const ByteSize&, const ByteSize&) noexcept = default;

 private:
  constexpr ByteSize(Kind kind, std::uint64_t value) noexcept : kind_(kind), value_(value) {}

  Kind kind_;
  std::uint64_t value_;
};

/// Parse a memory footprint: "1024", "512MiB", "8GiB", or "70%".
///
/// Binary units only (KiB/MiB/GiB/TiB). Decimal units are not accepted, because
/// a memory-sizing mistake of 7% is exactly the kind that goes unnoticed until
/// a run starts swapping.
[[nodiscard]] Result<ByteSize, ParseError> parse_byte_size(std::string_view text);

/// Render in the form parse_byte_size accepts, choosing the largest unit that
/// divides exactly so the result round-trips.
[[nodiscard]] std::string to_string(ByteSize size);

}  // namespace loadforge::core

#endif
