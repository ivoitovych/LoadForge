// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_REPORTERS_CSV_CSV_TEXT_HPP
#define LOADFORGE_REPORTERS_CSV_CSV_TEXT_HPP

// A document tree as CSV (RFC 4180): one row per leaf value, records ended by
// CRLF, a header row, a field quoted when it holds a comma, a quote, a CR or
// an LF and its quotes doubled.
//
// Columns:
//   schema_version  the result's schema version, on every row.
//   field           the leaf's path: members joined by ".", array items as
//                   "[index]" ("workloads[0].tally.oracle_blocks").
//   value           the value, or an empty cell when there is none.
//   status          "present"; or why the cell is empty: "absent" (not
//                   recorded), "empty" (an empty list or object), or the
//                   unavailable reading's kind ("absent", "vanished", "denied",
//                   "malformed", "unreadable").
//   reason          the status in words: "not recorded", "no items", "not
//                   permitted" ...; empty for a present value.
//   source_path     for an unavailable reading, the file or source that failed.
//   detail          for an unavailable reading, what failed, verbatim.
//   remediation     for an unavailable reading, what to do about it, verbatim.

#include <string>
#include <string_view>

#include "controller/results/document.hpp"

namespace loadforge::reporters {

/// One field, quoted if it needs it.
[[nodiscard]] std::string csv_field(std::string_view text);

/// The tree as a CSV table with its header row.
///
/// The root must be an object whose first member is "schema_version".
[[nodiscard]] std::string to_csv(const results::Node& document);

}  // namespace loadforge::reporters

#endif
