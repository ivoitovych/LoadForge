// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_RESOURCES_DECIMAL_HPP
#define LOADFORGE_RESOURCES_DECIMAL_HPP

#include <cstdint>
#include <optional>
#include <string_view>

namespace loadforge::resources {

/// An unsigned decimal exactly as the kernel prints one with "%lu" or "%llu":
/// one or more digits and nothing else -- no sign, no space, no '+', no hex.
/// Nothing when the text is anything else, or when the value does not fit in
/// 64 bits.
///
/// Every number this module reads -- a kB count in /proc/meminfo, a byte limit
/// in a cgroup file, an OOM-kill count, a hierarchy id -- is printed that way,
/// so one strict reader serves them all and a file that is not what we think
/// is refused rather than read as a plausible number. The bound is checked
/// BEFORE the multiply (journal §4.6): a value that left the range cannot be
/// inspected afterwards.
[[nodiscard]] std::optional<std::uint64_t> parse_decimal(std::string_view text) noexcept;

}  // namespace loadforge::resources

#endif
