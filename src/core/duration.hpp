// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CORE_DURATION_HPP
#define LOADFORGE_CORE_DURATION_HPP

#include <chrono>
#include <string>
#include <string_view>

#include "core/parse_error.hpp"
#include "core/result.hpp"

namespace loadforge::core {

using Duration = std::chrono::milliseconds;

/// Parse a phase duration as written in a configuration file.
///
/// The form is a non-negative integer followed by a unit, as used throughout
/// the design draft (§37): "500ms", "30s", "20m", "5h". Surrounding whitespace
/// is tolerated because TOML values are frequently padded.
///
/// Compound forms such as "1h30m" are deliberately not accepted: they invite
/// ambiguity for no benefit when "90m" says the same thing.
[[nodiscard]] Result<Duration, ParseError> parse_duration(std::string_view text);

/// Render a duration in the same form parse_duration accepts, choosing the
/// largest unit that divides exactly so the result round-trips.
[[nodiscard]] std::string to_string(Duration duration);

}  // namespace loadforge::core

#endif
