// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_RECORD_FORMAT_HPP
#define LOADFORGE_VERIFICATION_RECORD_FORMAT_HPP

// How a record's values are written for a person: the same spelling in a
// decode error's detail and in a rendered record, on every machine.

#include <cstdint>
#include <string>
#include <string_view>

namespace loadforge::verification {

/// The shortest decimal that reads back as exactly `value` (std::to_chars),
/// so a residual or a tolerance is never rounded on its way to a report:
/// "3.5e-12", "0.1", "nan", "-inf". Independent of the locale.
[[nodiscard]] std::string shortest(double value);

/// "0x" and the lower-case hexadecimal digits, without leading zeros: "0x0",
/// "0x7f0012340000".
[[nodiscard]] std::string hex(std::uint64_t value);

/// Text from a record, safe to print: printable ASCII as it is, a backslash
/// doubled, every other byte -- control characters, DEL, anything outside
/// ASCII -- as \xNN. A record's text arrived over a channel whose job is to
/// carry corruption; it must not be able to move a terminal's cursor or hide
/// what the report says.
[[nodiscard]] std::string escaped(std::string_view text);

}  // namespace loadforge::verification

#endif
