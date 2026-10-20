// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_CLI_HPP
#define LOADFORGE_MAIN_CLI_HPP

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "core/version.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "topology/snapshot.hpp"

namespace loadforge::cli {

/// Re-exported from the generated header so there is exactly one place a
/// version can be wrong.
inline constexpr std::string_view kVersion = core::kVersion;

/// Convert a raw argv span into arguments, dropping argv[0].
///
/// Lives here rather than in main() so the argc == 0 case is testable. POSIX
/// permits execve with an empty argv, and an unguarded subspan(1) on an empty
/// span is undefined behaviour -- which this project refuses to rely on
/// anywhere. A defensive branch that no test can reach is a branch nobody has
/// shown to work.
[[nodiscard]] std::vector<std::string_view> collect_args(std::span<char* const> argv);

/// Run the command line. Separated from main() so every path is testable
/// in-process; main() is a shim covered by a smoke test that runs the binary.
///
/// `args` excludes argv[0]. Returns the process exit code.
[[nodiscard]] int run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

/// The stress command: loads the configuration, refuses a mode this build
/// does not run, resolves the worker executable from `self_link` (always
/// /proc/self/exe in `run`; a test names a link that does not resolve), takes
/// the topology snapshot from `sysfs_root` (always /sys in `run`; a test names
/// a tree that refuses), and hands the run to run_stress. A machine whose
/// topology cannot be described is refused before any worker exists: the
/// snapshot is the frame the run is reported against (docs/PLAN.md §4.1).
[[nodiscard]] int stress(std::string_view config_path, const std::string& self_link,
                         std::string_view sysfs_root, std::ostream& out, std::ostream& err);

/// What a run is given beyond a configuration: the choices the command line
/// has and a configuration file does not (yet).
///
///   isa              the ISA path asked for; none: the best the machine and
///                    each workload offer.
///   seed             the run's seed; worker i runs seed + i * controller::kSeedStride.
///   footprint_bytes  a worker's footprint when its configuration names none.
///   isolation_limit  the most wrong results of one worker that fault isolation repeats.
struct StressOptions {
  /// Whether the telemetry sources are probed and the capability report is
  /// stated in the banner. Always, for a real run; a test that runs over a
  /// fake of the system calls turns it off, because a fake that answers every
  /// path with the same file gives the hwmon scan (which walks every chip
  /// number) thousands of chips to read.
  bool capabilities = true;
  std::optional<simd::IsaPath> isa;
  std::uint64_t seed = 1;
  std::uint64_t footprint_bytes = std::uint64_t{32} << 20U;
  std::size_t isolation_limit = 16;
};

/// The stress command for one named workload: `loadforge stress --test NAME
/// [options]`. `args` starts at NAME. A workload this build does not have, or
/// an option misused, is a usage error that says which. One worker per CPU the
/// process may use, unless told otherwise, each pinned to its own.
[[nodiscard]] int stress_test(std::span<const std::string_view> args, const std::string& self_link,
                              std::string_view sysfs_root, std::ostream& out, std::ostream& err);

/// The stress command past its argument handling: a loaded configuration,
/// the executable the workers will be, the topology snapshot already taken,
/// and the seam they run on. A reference, so no run can start without the
/// machine described before its first fork. It reads the
/// memory budget and opens the OOM-kill record through that seam, so the
/// controller checks memory against a real ceiling and can tell an OOM kill
/// from any other SIGKILL; memory is reported NOT ENFORCED only when the
/// budget cannot be read, and the reason goes with it. Public so
/// that the paths a real binary cannot take -- a fork that fails, an exec
/// that fails and returns a worker's exit code, a run that ends unclean --
/// are driven with a fake; `run` calls it with the real seam and this
/// executable, resolved from /proc/self/exe.
[[nodiscard]] int run_stress(platform::Syscalls& syscalls, const config::Config& config,
                             const std::string& executable, const topology::Snapshot& snapshot,
                             const StressOptions& options, std::ostream& out, std::ostream& err);

}  // namespace loadforge::cli

#endif
