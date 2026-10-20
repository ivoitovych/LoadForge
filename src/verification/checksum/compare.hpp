// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_CHECKSUM_COMPARE_HPP
#define LOADFORGE_VERIFICATION_CHECKSUM_COMPARE_HPP

// Exact comparison (verification.md §5, `exact`): expected bytes against
// actual bytes, into the Difference that record.hpp defines -- first and last
// differing byte, the popcount over the whole comparison, the window from the
// first differing word -- and from there into the error record a caller sends.
//
// FOUR OUTCOMES, BECAUSE "NOT EQUAL" IS TWO FINDINGS AND "EQUAL" CAN BE NONE
// --------------------------------------------------------------------------
//   kMatch            every byte compared, at least one, and all equal.
//   kMismatch         same length, some byte differs: a Difference.
//   kLengthMismatch   the two sides are not the same length -- a decoded
//                     stream one byte short, say. A wrong result with no
//                     byte-for-byte difference to classify; the record says
//                     so rather than comparing a prefix and inventing bits.
//   kNothingCompared  both sides empty. Comparing zero bytes is not a pass
//                     (verification.md §5): it is VerificationNotPerformed.
//
// OFFSETS AND THE HARDWARE'S BOUNDARIES
// -------------------------------------
// Offsets count from the first byte of the spans. For Domain::kStored the
// caller passes spans that start at the buffer the provenance names, which the
// runner places on a cache-line boundary (record.hpp, Provenance); only then
// do the word and line boundaries of the classification fall where the
// hardware's do. This function cannot see an address and does not pretend to.
//
// DETERMINISM: pure functions of their arguments. THREAD-SAFETY: pure.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "verification/record.hpp"

namespace loadforge::verification {

enum class ComparisonOutcome : std::uint8_t {
  kMatch = 0,
  kMismatch = 1,
  kLengthMismatch = 2,
  kNothingCompared = 3,
};

[[nodiscard]] std::string_view describe(ComparisonOutcome outcome) noexcept;

/// What compare_exact() found. `difference` is set exactly when the outcome
/// is kMismatch; the two lengths are always the spans' own.
struct Comparison {
  ComparisonOutcome outcome = ComparisonOutcome::kNothingCompared;
  std::uint64_t expected_bytes = 0;
  std::uint64_t actual_bytes = 0;
  std::optional<Difference> difference;
};

[[nodiscard]] bool operator==(const Comparison& lhs, const Comparison& rhs) noexcept;

/// Compares `actual` with `expected` byte for byte. A Difference it returns is
/// valid by record.hpp's rules: popcount >= 1, first <= last, the window from
/// first_offset rounded down to a word, min(kMaxWindowBytes, length -
/// window_offset) bytes of each side.
[[nodiscard]] Comparison compare_exact(std::span<const std::byte> expected,
                                       std::span<const std::byte> actual, Domain domain);

/// What a record needs that the bytes do not say: where they were, which side
/// they are, what was compared, and the topology's cache-line size.
///
/// Preconditions, under which every record made from a Subject is valid
/// (is_valid()): `check` is not empty and leaves room in kMaxTextBytes for
/// the detail appended to it (kMaxCheckBytes), `line_bytes` is a power of two
/// and at least kWordBytes, and the provenance is itself valid.
struct Subject {
  Provenance provenance;
  Side side = Side::kOutput;
  std::string check;
  std::uint32_t line_bytes = 64;
};

/// The longest check a Subject may carry: what remains of kMaxTextBytes once
/// the longest detail this module appends has been added.
inline constexpr std::size_t kMaxCheckBytes = kMaxTextBytes - 256;

/// The record a comparison calls for, or nothing for kMatch:
///   kMismatch        ExactBitCorruption carrying the Difference.
///   kLengthMismatch  ExactBitCorruption with no difference, its check naming
///                    both lengths.
///   kNothingCompared VerificationNotPerformed, blocks_compared 0.
[[nodiscard]] std::optional<ErrorRecord> to_record(const Comparison& comparison,
                                                   const Subject& subject);

}  // namespace loadforge::verification

#endif
