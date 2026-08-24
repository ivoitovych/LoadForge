// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CORE_BYTE_SIZE_HPP
#define LOADFORGE_CORE_BYTE_SIZE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

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
///
/// Invalid states are unrepresentable rather than merely undocumented: a
/// percentage outside 1..100 cannot be constructed at all, and asking an
/// absolute size for its percentage throws rather than silently returning the
/// byte count. Both follow the rule Result already establishes -- defined
/// failure, never a plausible wrong answer.
class ByteSize {
 public:
  enum class Kind : std::uint8_t { Absolute, Fraction };

  static constexpr ByteSize bytes(std::uint64_t count) noexcept { return ByteSize{Bytes{count}}; }

  /// A share of available memory, in whole percent. Rejects 0 and anything
  /// above 100: neither is a meaningful footprint, and both were constructible
  /// before even though the parser refused them.
  [[nodiscard]] static Result<ByteSize, ParseError> percent(std::uint64_t whole) {
    if (whole == 0 || whole > kMaxPercent) {
      return ParseError::OutOfRange;
    }
    return ByteSize{Fraction{whole}};
  }

  [[nodiscard]] constexpr Kind kind() const noexcept {
    return std::holds_alternative<Bytes>(store_) ? Kind::Absolute : Kind::Fraction;
  }
  [[nodiscard]] constexpr bool is_absolute() const noexcept { return kind() == Kind::Absolute; }

  /// Throws std::bad_variant_access if this is a percentage.
  [[nodiscard]] constexpr std::uint64_t byte_count() const { return std::get<Bytes>(store_).count; }

  /// In whole percent, 1..100. Throws std::bad_variant_access if this is absolute.
  [[nodiscard]] constexpr std::uint64_t percent_of_available() const {
    return std::get<Fraction>(store_).whole;
  }

  friend constexpr bool operator==(const ByteSize&, const ByteSize&) noexcept = default;

  static constexpr std::uint64_t kMaxPercent = 100;

 private:
  struct Bytes {
    std::uint64_t count;
    friend constexpr bool operator==(const Bytes&, const Bytes&) noexcept = default;
  };
  struct Fraction {
    std::uint64_t whole;
    friend constexpr bool operator==(const Fraction&, const Fraction&) noexcept = default;
  };

  constexpr explicit ByteSize(std::variant<Bytes, Fraction> store) noexcept : store_(store) {}

  std::variant<Bytes, Fraction> store_;
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
