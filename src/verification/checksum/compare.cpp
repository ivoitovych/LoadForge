// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/checksum/compare.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "verification/record.hpp"

namespace loadforge::verification {

std::string_view describe(ComparisonOutcome outcome) noexcept {
  switch (outcome) {
    case ComparisonOutcome::kMatch:
      return "match";
    case ComparisonOutcome::kMismatch:
      return "mismatch";
    case ComparisonOutcome::kLengthMismatch:
      return "length mismatch";
    case ComparisonOutcome::kNothingCompared:
      return "nothing compared";
  }
  return "unrecognised comparison outcome";
}

bool operator==(const Comparison& lhs, const Comparison& rhs) noexcept {
  return std::tie(lhs.outcome, lhs.expected_bytes, lhs.actual_bytes, lhs.difference) ==
         std::tie(rhs.outcome, rhs.expected_bytes, rhs.actual_bytes, rhs.difference);
}

Comparison compare_exact(std::span<const std::byte> expected, std::span<const std::byte> actual,
                         Domain domain) {
  Comparison comparison;
  comparison.expected_bytes = expected.size();
  comparison.actual_bytes = actual.size();
  if (expected.size() != actual.size()) {
    comparison.outcome = ComparisonOutcome::kLengthMismatch;
    return comparison;
  }
  if (expected.empty()) {
    comparison.outcome = ComparisonOutcome::kNothingCompared;
    return comparison;
  }
  // The common case, every byte equal, at memcmp's speed; the search for
  // where they differ runs only when they do.
  if (std::memcmp(expected.data(), actual.data(), expected.size()) == 0) {
    comparison.outcome = ComparisonOutcome::kMatch;
    return comparison;
  }
  const std::size_t first =
      static_cast<std::size_t>(std::ranges::mismatch(expected, actual).in1 - expected.begin());
  std::size_t last = expected.size() - 1;
  while (expected[last] == actual[last]) {
    --last;
  }
  std::uint64_t popcount = 0;
  for (std::size_t at = first; at <= last; ++at) {
    popcount += static_cast<std::uint64_t>(
        std::popcount(std::to_integer<unsigned>(expected[at] ^ actual[at])));
  }
  Difference difference;
  difference.domain = domain;
  difference.first_offset = first;
  difference.last_offset = last;
  difference.popcount = popcount;
  difference.window_offset = first / kWordBytes * kWordBytes;
  const std::size_t window_start = difference.window_offset;
  const std::size_t window = std::min(kMaxWindowBytes, expected.size() - window_start);
  difference.expected.assign(expected.begin() + static_cast<std::ptrdiff_t>(window_start),
                             expected.begin() + static_cast<std::ptrdiff_t>(window_start + window));
  difference.actual.assign(actual.begin() + static_cast<std::ptrdiff_t>(window_start),
                           actual.begin() + static_cast<std::ptrdiff_t>(window_start + window));
  comparison.outcome = ComparisonOutcome::kMismatch;
  comparison.difference = std::move(difference);
  return comparison;
}

// One allocation per statement: an aggregate built from several allocating
// members in one expression leaves a cleanup block whose branches no test can
// enter (journal §1.13).
VerificationNotPerformed not_performed(const Subject& subject, std::string why) {
  VerificationNotPerformed record;
  record.provenance = subject.provenance;
  record.what = subject.check;
  record.why = std::move(why);
  return record;
}

std::optional<ErrorRecord> to_record(const Comparison& comparison, const Subject& subject) {
  if (comparison.outcome == ComparisonOutcome::kMatch) {
    return std::nullopt;
  }
  if (comparison.outcome == ComparisonOutcome::kNothingCompared) {
    return not_performed(subject, "there were no bytes to compare: both sides were empty");
  }
  ExactBitCorruption record;
  record.provenance = subject.provenance;
  record.side = subject.side;
  record.check = subject.check;
  record.line_bytes = subject.line_bytes;
  if (comparison.outcome == ComparisonOutcome::kLengthMismatch) {
    record.check += " -- expected " + std::to_string(comparison.expected_bytes) + " bytes, found " +
                    std::to_string(comparison.actual_bytes) +
                    ": no byte-for-byte difference to classify";
  } else {
    record.difference = comparison.difference;
  }
  return record;
}

}  // namespace loadforge::verification
