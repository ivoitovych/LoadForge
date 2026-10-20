// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_REPORTERS_JSON_JSON_TEXT_HPP
#define LOADFORGE_REPORTERS_JSON_JSON_TEXT_HPP

// A document tree as JSON text (RFC 8259): two-space indentation, members in
// the order they were added, "\n" line ends and a closing newline, so equal
// trees give byte-identical text.

#include <cstddef>
#include <string>
#include <string_view>

#include "controller/results/document.hpp"

namespace loadforge::reporters {

/// The string as a JSON string literal, quotes included. Every control
/// character is escaped. A byte sequence that is not UTF-8 -- a kernel string
/// is whatever bytes the driver wrote -- is replaced by U+FFFD, one per
/// offending byte, so the document stays valid UTF-8 and nothing is dropped
/// silently.
[[nodiscard]] std::string json_string(std::string_view text);

/// The length of the UTF-8 sequence that starts `text`, or 0 when it does not
/// start one (RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF).
[[nodiscard]] std::size_t utf8_sequence_length(std::string_view text) noexcept;

/// The tree as JSON.
///
/// An Absence that is the value of an object member is written as null, and
/// the reason follows as a member of its own named "<member>_unavailable":
/// {"kind", "words", "path", "detail", "remediation"}. An Absence anywhere else is
/// that object itself.
[[nodiscard]] std::string to_json(const results::Node& document);

}  // namespace loadforge::reporters

#endif
