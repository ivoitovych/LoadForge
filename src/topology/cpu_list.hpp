// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TOPOLOGY_CPU_LIST_HPP
#define LOADFORGE_TOPOLOGY_CPU_LIST_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"

namespace loadforge::topology {

/// Why a value the kernel reported could not be interpreted.
///
/// Deliberately NOT core::ParseError, which documents itself as being about a
/// textual *configuration* value -- something a user wrote and can fix. This is
/// the other direction: the machine reported something, and the remedy is not
/// "correct your file" but "this kernel is not what this code expects".
/// Reporting the two with one type would put the wrong advice in front of the
/// wrong reader.
struct MalformedValue {
  std::string_view reason;  ///< What was wrong. Always a literal.
  std::string value;        ///< The text as read, so a report can quote it.

  [[nodiscard]] friend bool operator==(const MalformedValue&, const MalformedValue&) = default;
};

/// Human-readable rendering, suitable for putting in front of a user.
[[nodiscard]] std::string describe(const MalformedValue& error);

/// A set of CPU ids, in the format Linux writes throughout /sys.
///
/// THE FORMAT, AND WHY THE PARSER IS STRICT ABOUT IT
/// ------------------------------------------------
/// Comma-separated elements, each either a single id or an inclusive range:
///
///     0-3            every CPU on this machine
///     0,2-4,7        what `online` looks like with CPUs 1, 5 and 6 offline
///     (empty)        what `offline` looks like when none are
///
/// The empty string is a LEGITIMATE READING, not an error. `offline` is empty on
/// a healthy machine, and a parser that rejected it would fail on the ordinary
/// case (F3: absent, empty and malformed are three different things).
///
/// Everything else is refused, including forms that could plausibly be guessed
/// at -- a descending range, a duplicate id, elements out of order. The kernel
/// writes these files with cpumask_print_to_pagebuf, which always emits
/// ascending, non-overlapping, non-repeating elements. So a file that breaks
/// those rules is not a sloppy CPU list to be tidied up; it is evidence that
/// this is not the file we think it is, and guessing would silently produce a
/// topology the machine does not have.
class CpuList {
 public:
  /// Largest id accepted. Linux caps CONFIG_NR_CPUS at 8192 on x86-64 and 4096
  /// on arm64, so this is ample headroom -- and a value past it means the text
  /// is not a CPU list, which is worth refusing rather than allocating for.
  static constexpr std::uint32_t kMaxCpuId = 65535;

  CpuList() = default;

  /// Parses one line of sysfs CPU-list text.
  ///
  /// Expects the value already stripped of its trailing newline, which is what
  /// platform::FileSystem::read_first_line returns.
  [[nodiscard]] static core::Result<CpuList, MalformedValue> parse(std::string_view text);

  /// The ids, ascending. Guaranteed sorted and unique by the parser's strictness.
  [[nodiscard]] const std::vector<std::uint32_t>& ids() const noexcept { return ids_; }

  [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }
  [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }
  [[nodiscard]] bool contains(std::uint32_t id) const noexcept;

  [[nodiscard]] friend bool operator==(const CpuList&, const CpuList&) = default;

 private:
  explicit CpuList(std::vector<std::uint32_t> ids) : ids_(std::move(ids)) {}

  std::vector<std::uint32_t> ids_;
};

}  // namespace loadforge::topology

#endif
