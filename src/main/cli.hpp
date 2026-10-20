// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_CLI_HPP
#define LOADFORGE_MAIN_CLI_HPP

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "core/result.hpp"
#include "core/version.hpp"
#include "main/result_files.hpp"
#include "main/telemetry_rig.hpp"
#include "platform/fpenv/fpenv.hpp"
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

/// What `loadforge stress` is asked for beyond its configuration.
///
///   formats          the result formats to write ("json", "csv"); none writes
///                    no file, and the run is reported on the console alone.
///   output_dir       where "run.<format>" goes.
///   sample_interval_ns   how often the sampler reads every source.
///   roots            where the machine's files are: the running machine's.
///   host             what the machine's registers say that no file does.
///   sink             where the result's bytes go; null is the filesystem.
struct HostFacts {
  std::optional<fpenv::FpState> fp_environment;
  std::optional<simd::IsaPath> isa_path;
};

/// The floating-point state of this thread and the ISA path this machine would
/// select, as the fingerprint records them.
[[nodiscard]] HostFacts read_host_facts();

struct StressOptions {
  std::vector<std::string> formats;
  std::string output_dir = ".";
  std::int64_t sample_interval_ns = 1'000'000'000;
  MachineRoots roots;
  std::optional<HostFacts> host;  ///< Null: read_host_facts().
  ResultSink* sink = nullptr;
  /// False runs the supervisor alone: no source is probed, no round taken. The
  /// command line never says it; the tests of the memory budget and the OOM
  /// record, which script the seam's reads in the order those readers make
  /// them, do.
  bool sample = true;
};

/// The stress command's arguments after "stress": `--config FILE`, and
/// optionally `--output FORMATS`, `--output-dir DIR` and `--sample-interval
/// DURATION`, in any order, each once.
struct StressArgs {
  std::string config_path;
  StressOptions options;
};

[[nodiscard]] core::Result<StressArgs, std::string> parse_stress_args(
    std::span<const std::string_view> args);

/// The stress command: loads the configuration, refuses a mode this build
/// does not run, resolves the worker executable from `self_link` (always
/// /proc/self/exe in `run`; a test names a link that does not resolve), takes
/// the topology snapshot from `sysfs_root` (always /sys in `run`; a test names
/// a tree that refuses), and hands the run to run_stress. A machine whose
/// topology cannot be described is refused before any worker exists: the
/// snapshot is the frame the run is reported against (docs/PLAN.md §4.1).
[[nodiscard]] int stress(std::string_view config_path, const std::string& self_link,
                         std::string_view sysfs_root, const StressOptions& options,
                         std::ostream& out, std::ostream& err);

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
