// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_RESULTS_FINGERPRINT_HPP
#define LOADFORGE_CONTROLLER_RESULTS_FINGERPRINT_HPP

// The run fingerprint (PLAN F12), read from the machine through the platform
// seam. Every field the machine might not tell us is optional in the result;
// here is how each one is asked, and what is recorded when the machine does
// not answer.
//
// WHERE EACH FIELD COMES FROM
// ---------------------------
//   loadforge_version, git_commit, compiler, flags   the build (this_build()).
//   cpu_model, microcode      /proc/cpuinfo, "model name" and "microcode":
//                             x86 prints both; arm64's cpuinfo prints
//                             neither, and the fields are then absent.
//   kernel                    /proc/sys/kernel/osrelease.
//   smt_enabled               /sys/devices/system/cpu/smt/active.
//   power_source              the mains adapter's `online`, under
//                             /sys/class/power_supply/{AC,ACAD,ADP0,ADP1,AC0}:
//                             1 is "ac", 0 is "battery". The seam cannot list
//                             a directory, so these are the names laptops use.
//   battery_state             the `status` of BAT0 or BAT1, in the kernel's own
//                             words ("Charging", "Discharging", "Full").
//   cpufreq_governor, energy_performance_preference
//                             cpu0's cpufreq directory.
//   platform_profile          /sys/firmware/acpi/platform_profile.
//   turbo_enabled             intel_pstate/no_turbo (0 is turbo on), else
//                             cpufreq/boost (1 is turbo on).
//   cooling_profile           the policy of thermal zone 0.
//   cgroup_memory_limit_bytes the limit that binds the memory cgroup
//                             (resources::read_memory_budget).
//   huge_page_policy          the bracketed word of
//                             /sys/kernel/mm/transparent_hugepage/enabled.
//   temperature_at_start_c    thermal zone 0's temp, in millidegrees.
//   fp_environment, isa_path  not on the machine's files: the caller read
//                             them (FpControl::read(), the selected path).
//
// ABSENT, AND WHY
// ---------------
// A field that cannot be read is absent in the result, and the reading says
// why in `gaps`: the topology capability model's state (absent, denied,
// malformed, unreadable), the path, and the detail. A field is never a
// default value standing in for a reading.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "controller/results/result.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "platform/fs.hpp"
#include "primitives/simd/dispatch.hpp"
#include "topology/source.hpp"

namespace loadforge::results {

/// What the build itself says. Fixed when the program was built.
struct BuildInfo {
  std::string loadforge_version;
  std::optional<std::string> git_commit;  ///< Absent outside a git checkout.
  std::string compiler;
  std::string flags;
};

/// This program's own build.
[[nodiscard]] BuildInfo this_build();

/// The text, or nothing when it is empty: how a build with no commit says so.
[[nodiscard]] std::optional<std::string> non_empty(std::string_view text);

/// What the caller knows that no file says.
struct FingerprintContext {
  BuildInfo build;
  std::optional<fpenv::FpState> fp_environment;
  std::optional<simd::IsaPath> isa_path;
};

/// Why one field is absent.
struct FingerprintGap {
  std::string field;  ///< The Fingerprint member's name.
  topology::Unavailable why;
};

struct FingerprintReading {
  Fingerprint fingerprint;
  std::vector<FingerprintGap> gaps;  ///< In field order.
};

/// Reads the fingerprint. `root` is empty on the running machine; a test
/// passes a directory it built, and every path moves with it.
[[nodiscard]] FingerprintReading read_fingerprint(platform::FileSystem& filesystem,
                                                  std::string_view root,
                                                  const FingerprintContext& context);

/// The value of `key` in the lines of /proc/cpuinfo ("key<spaces>: value"),
/// the first CPU's. Nothing when no line has the key.
[[nodiscard]] std::optional<std::string> cpuinfo_value(const std::vector<std::string>& lines,
                                                       std::string_view key);

/// The word in square brackets in a kernel selector line such as
/// "always [madvise] never". Nothing when there is not exactly one.
[[nodiscard]] std::optional<std::string> selected_word(std::string_view line);

/// The fingerprint fields that differ between two fingerprints, in field
/// order, by the Fingerprint member's name.
[[nodiscard]] std::vector<std::string_view> differing_fields(const Fingerprint& lhs,
                                                             const Fingerprint& rhs);

/// Fields that describe the moment and not the machine: two runs a minute
/// apart on one machine may differ in them (the battery charges, the die
/// warms) and still share the operating context F12 asks for.
[[nodiscard]] bool is_moment_field(std::string_view field) noexcept;

}  // namespace loadforge::results

#endif
