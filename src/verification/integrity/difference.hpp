// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_INTEGRITY_DIFFERENCE_HPP
#define LOADFORGE_VERIFICATION_INTEGRITY_DIFFERENCE_HPP

// The bracket's comparison: a unit's input as stored in its slot against the
// same unit regenerated, byte for byte. Its difference is the evidence an
// input ExactBitCorruption carries (record.hpp), so it keeps that type's
// rules exactly: offsets from the start of the input, which the runner places
// on a line boundary; the popcount over the whole input, not the window; the
// window from the first differing byte rounded down to a word, at most
// kMaxWindowBytes long and never past the end of the input.

#include <cstddef>
#include <optional>
#include <span>

#include "verification/record.hpp"

namespace loadforge::verification::integrity {

/// What differs between `expected` (the regeneration) and `actual` (the
/// slot), as Domain::kStored, or nothing when the bytes are equal.
///
/// Precondition: the two spans have the same length -- both are one unit's
/// input, of the plan's size.
[[nodiscard]] std::optional<Difference> compare_stored(std::span<const std::byte> expected,
                                                       std::span<const std::byte> actual);

}  // namespace loadforge::verification::integrity

#endif
