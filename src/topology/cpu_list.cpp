// SPDX-License-Identifier: GPL-3.0-or-later
#include "topology/cpu_list.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"

namespace loadforge::topology {
namespace {

/// Parses a run of decimal digits into an id, or says why it could not.
///
/// Deliberately not strtoul or from_chars-with-a-shrug: it accepts DIGITS ONLY.
/// strtoul would skip leading whitespace, accept a leading '+' or '-', and
/// happily read "0x10" as 16 -- none of which the kernel ever writes, and each
/// of which would turn a malformed file into a plausible wrong answer.
/// Returns the id, or the REASON it could not be read -- not a full
/// MalformedValue.
///
/// Taking only the element, and leaving the caller to attach the whole value it
/// came from, is deliberate. The obvious signature is
/// `parse_id(element, whole_value)`, two adjacent string_views that a caller can
/// silently transpose: the result parses the entire list as one id and quotes
/// the element as the offending text. Separating the concerns -- this decides
/// WHAT is wrong, the caller knows WHAT THE VALUE WAS -- makes that mistake
/// unwritable rather than merely unlikely.
core::Result<std::uint32_t, std::string_view> parse_id(std::string_view text) {
  if (text.empty()) {
    return std::string_view{"an element is missing its number"};
  }
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return std::string_view{"an element contains something that is not a digit"};
    }
    value = value * 10 + static_cast<std::uint64_t>(digit - '0');
    // Checked inside the loop, not after: a long enough run of digits would
    // wrap a 64-bit accumulator and land back on a small, entirely plausible id.
    if (value > CpuList::kMaxCpuId) {
      return std::string_view{"an id is larger than any CPU this kernel can have"};
    }
  }
  return static_cast<std::uint32_t>(value);
}

}  // namespace

std::string describe(const MalformedValue& error) {
  return "cannot interpret \"" + error.value + "\": " + std::string{error.reason};
}

core::Result<CpuList, MalformedValue> CpuList::parse(std::string_view text) {
  // Empty is a real reading: `offline` is empty on a machine with every CPU up.
  // Returning an empty list here rather than an error is what lets a caller
  // distinguish "no CPUs are offline" from "I could not find out".
  if (text.empty()) {
    return CpuList{};
  }

  std::vector<std::uint32_t> ids;
  std::size_t position = 0;
  bool have_previous = false;
  std::uint32_t previous_end = 0;

  // `while (true)`, not a bound on `position`: the ONLY way out is the break
  // below, when an element has no comma after it. A loop condition that can
  // never be false is worse than none -- it reads as a safety check nobody has
  // to think about, and the coverage gate correctly refused to call it covered.
  while (true) {
    const std::size_t comma = text.find(',', position);
    const std::string_view element = text.substr(
        position, comma == std::string_view::npos ? std::string_view::npos : comma - position);

    const std::size_t dash = element.find('-');
    auto first = parse_id(element.substr(0, dash));
    if (!first) {
      return MalformedValue{first.error(), std::string{text}};
    }
    std::uint32_t last = first.value();
    if (dash != std::string_view::npos) {
      auto parsed_last = parse_id(element.substr(dash + 1));
      if (!parsed_last) {
        return MalformedValue{parsed_last.error(), std::string{text}};
      }
      last = parsed_last.value();
      // "3-0" is not an empty range to skip over, nor a range to helpfully
      // reverse. cpumask_print_to_pagebuf cannot produce it.
      if (last < first.value()) {
        return MalformedValue{"a range ends before it begins", std::string{text}};
      }
    }

    // Ascending and non-overlapping, enforced ACROSS elements. "0,0" and "2-4,3"
    // and "3,1" are all refused for the same reason: the kernel's own formatter
    // cannot emit them, so a file containing one is not the file we think.
    if (have_previous && first.value() <= previous_end) {
      return MalformedValue{"elements are not in ascending order, or they overlap",
                            std::string{text}};
    }

    for (std::uint32_t id = first.value(); id <= last; ++id) {
      ids.push_back(id);
    }
    have_previous = true;
    previous_end = last;

    if (comma == std::string_view::npos) {
      break;
    }
    position = comma + 1;
    // A trailing comma leaves an empty element, which parse_id refuses on the
    // next turn -- so "0,3," fails rather than silently parsing as "0,3".
  }

  return CpuList{std::move(ids)};
}

bool CpuList::contains(std::uint32_t id) const noexcept {
  return std::binary_search(ids_.begin(), ids_.end(), id);
}

}  // namespace loadforge::topology
