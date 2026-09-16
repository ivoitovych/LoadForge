// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TOPOLOGY_CPU_TOPOLOGY_HPP
#define LOADFORGE_TOPOLOGY_CPU_TOPOLOGY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cpu_list.hpp"
#include "topology/source.hpp"

namespace loadforge::topology {

/// Where the kernel publishes what the machine is.
///
/// A parameter rather than a hard-coded constant, so a test can point discovery
/// at a tree it built. That is not a testing hook bolted on: the tree is the
/// only way to reach most of what this module decides, because a real machine
/// offers exactly one topology and it is a consistent one.
inline constexpr std::string_view kDefaultSysfsRoot = "/sys";

/// Which physical core a logical CPU sits on.
///
/// Both halves are needed. `core_id` is unique only WITHIN a package -- two
/// packages each have a core 0 -- so a core identified by `core_id` alone would
/// merge every socket's core 0 into one and report half the machine.
struct CoreAddress {
  std::uint32_t package = 0;
  std::uint32_t core = 0;

  [[nodiscard]] friend bool operator==(const CoreAddress&, const CoreAddress&) = default;
};

/// The pair as one value, for counting distinct cores.
///
/// Exact rather than a hash: both halves are 32-bit, so a 64-bit key holds them
/// with nothing to collide. A hash would need a collision story, and a collision
/// here would silently merge two cores into one -- in the number every later
/// report is attributed against.
///
/// This exists instead of a defaulted `operator<=>`, which is the obvious way to
/// make the type sortable and was the first attempt. Nothing needed an ORDER on
/// a core address, only a way to tell distinct ones apart, so the defaulted
/// operator contributed comparison arms no caller could reach and the coverage
/// gate correctly refused to call them covered. Deleting unreachable code beats
/// excusing it (the exclusion ladder, rung 3).
///
/// A free function rather than a member, following describe(MalformedValue) and
/// describe(Unavailable): every value type in this module stays a plain
/// aggregate whose operations live beside it. Adding the first member function
/// would also have made clang-tidy's public-member check fire, correctly --
/// a struct with methods and public data is neither one thing nor the other.
[[nodiscard]] constexpr std::uint64_t key(const CoreAddress& address) noexcept {
  return (static_cast<std::uint64_t>(address.package) << 32U) | address.core;
}

/// One logical CPU, as the kernel describes it.
struct LogicalCpu {
  std::uint32_t id = 0;
  CoreAddress core;
  /// Every CPU sharing this one's physical core, including this CPU itself.
  /// On a machine without SMT that is the CPU alone, which is a real answer
  /// rather than a degenerate one.
  CpuList thread_siblings;
};

/// Why the machine's topology could not be established.
///
/// TWO FAILURES THAT MUST NOT BE COLLAPSED
/// ---------------------------------------
/// A source that would not answer and a set of sources that all answered and
/// contradict each other are different events with different remedies. The
/// first is "I could not find out" -- a permission, a kernel that does not
/// offer the attribute, a device that went away. The second is "I found out
/// twice and got two incompatible answers", which means one of the readings is
/// not describing this machine, and no amount of permission fixes it.
///
/// Reporting the second as the first would send a user hunting for an access
/// problem that does not exist. Reporting the first as the second would accuse
/// their kernel of lying.
///
/// The suppression is the third instance of the one platform::SyscallError and
/// topology::Unavailable carry, and it is narrow for the same provable reason:
/// clang-analyzer claims `kind` can be garbage when this struct is copied out of
/// a Result, because it cannot follow a value through std::variant and treats
/// the held alternative as unconstructed. Every field has a default member
/// initialiser, so even a default-constructed DiscoveryFailure has defined
/// values and the condition the check describes cannot occur for this type. The
/// copy is asserted intact, field by field, by
/// CpuTopologyTest.AMissingCoreIdIsASourceFailure.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct DiscoveryFailure {
  enum class Kind : std::uint8_t {
    kSource,        ///< A source would not answer; `availability` says how.
    kContradiction  ///< Every source answered; they cannot all be true.
  };

  Kind kind = Kind::kSource;
  /// How the source failed. Carries a defined value in both cases -- it is not
  /// read for a contradiction, and a field that is sometimes garbage is a trap
  /// rather than an optimisation.
  Availability availability = Availability::kUnreadable;
  std::string subject;  ///< The path, or the CPUs that disagree.
  std::string detail;   ///< Already rendered, by whatever held the evidence.

  [[nodiscard]] friend bool operator==(const DiscoveryFailure&, const DiscoveryFailure&) = default;
};

/// Human-readable rendering, suitable for putting in front of a user.
[[nodiscard]] std::string describe(const DiscoveryFailure& failure);

/// What the machine's CPUs actually are: how many, on which cores, in which
/// packages, and whether any core carries more than one thread.
///
/// WHY DISCOVERY CAN FAIL ON A MACHINE WHERE EVERY FILE READS FINE
/// ---------------------------------------------------------------
/// The kernel publishes this information several times over, in files that must
/// agree: a CPU's thread siblings must include the CPU itself; two CPUs that
/// call each other siblings must report the same core; every sibling must be
/// online. Nothing enforces that from outside, and a machine where it does not
/// hold is a machine this code is misreading.
///
/// So discovery CROSS-CHECKS, and refuses when the checks fail rather than
/// picking whichever file it read first. That is the same judgement CpuList
/// makes about a malformed value and for the same reason: a plausible wrong
/// topology is worse than a reported failure, because every later number is
/// attributed against it and nothing downstream would think to question it. A
/// refusal is loud once; a wrong core count is quietly wrong in every report
/// the tool ever produces.
///
/// The agreement being checked is real evidence rather than a tautology (F21),
/// because the files are written by different parts of the kernel from
/// different internal structures -- `online` from the hotplug machinery,
/// `thread_siblings_list` from the scheduler's topology masks, `core_id` and
/// `physical_package_id` from the architecture's CPU identification. A bug in
/// this code cannot make them agree.
class CpuTopology {
 public:
  /// Read and cross-check the machine's CPU topology.
  [[nodiscard]] static core::Result<CpuTopology, DiscoveryFailure> discover(
      platform::FileSystem& filesystem, std::string_view sysfs_root = kDefaultSysfsRoot);

  /// The online CPUs, ascending by id.
  [[nodiscard]] const std::vector<LogicalCpu>& cpus() const noexcept { return cpus_; }

  [[nodiscard]] std::size_t logical_cpu_count() const noexcept { return cpus_.size(); }

  /// Distinct (package, core) pairs -- physical cores, not threads.
  [[nodiscard]] std::size_t physical_core_count() const noexcept;

  /// Distinct package ids -- sockets.
  [[nodiscard]] std::size_t package_count() const noexcept;

  /// Whether any physical core carries more than one logical CPU.
  ///
  /// Derived from the sibling sets rather than read from `smt/active`, which is
  /// a separate file that can be absent (it is not offered on every
  /// architecture) and whose `control` sibling reports the string
  /// "notsupported" -- verified on the machine this was written against. The
  /// derived answer is available wherever the topology itself is.
  [[nodiscard]] bool multithreaded() const noexcept;

 private:
  explicit CpuTopology(std::vector<LogicalCpu> cpus) : cpus_(std::move(cpus)) {}

  std::vector<LogicalCpu> cpus_;
};

}  // namespace loadforge::topology

#endif
