// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_RECORD_RENDER_HPP
#define LOADFORGE_VERIFICATION_RECORD_RENDER_HPP

// An error record as a person reads it (verification.md §8).
//
// Three rules the text keeps, because each is a way a report could tell the
// user something false about their hardware:
//
//   * Absence is printed as absence. A provenance field the producer left
//     unset reads "not recorded", never 0; an exact corruption without a
//     difference says it has no bit evidence; a bounded failure says its bit
//     fields do not apply.
//   * A classification of DERIVED bytes says so, beside the classification:
//     the bits it describes are the oracle's, not bits the hardware stored.
//   * VerificationNotPerformed says it is a failure of the run, not a pass,
//     and says nothing about the hardware.
//
// Text fields arrive from a channel built to carry corruption, so they are
// printed escaped (format.hpp).
//
// DETERMINISM: a pure function of the record; numbers are spelled without
// the locale (std::to_chars). THREAD-SAFETY: pure function.

#include <string>

#include "verification/record.hpp"

namespace loadforge::verification {

/// The record as lines of text, each ending in '\n'. The first line names the
/// kind; the provenance follows the kind's own fields.
[[nodiscard]] std::string render(const ErrorRecord& record);

}  // namespace loadforge::verification

#endif
