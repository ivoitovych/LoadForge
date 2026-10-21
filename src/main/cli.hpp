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
#include "controller/journal/file_ops.hpp"
#include "controller/results/result.hpp"
#include "core/result.hpp"
#include "core/version.hpp"
#include "main/journal_rig.hpp"
#include "main/outcome.hpp"
#include "main/result_files.hpp"
#include "main/telemetry_rig.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "statistics/signature.hpp"
#include "topology/snapshot.hpp"
#include "verification/runner.hpp"
#include "workloads/mixed/schedule.hpp"

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

/// What the machine's registers say that no file does.
struct HostFacts {
  std::optional<fpenv::FpState> fp_environment;
  std::optional<simd::IsaPath> isa_path;
};

/// The floating-point state of this thread and the ISA path this machine would
/// select, as the fingerprint records them.
[[nodiscard]] HostFacts read_host_facts();

/// What a run is given beyond a configuration.
///
/// What is written and how the machine is watched (M3):
///   formats          the result formats to write ("json", "csv"); none writes
///                    no file, and the run is reported on the console alone.
///   output_dir       where "run.<format>" goes.
///   sample_interval_ns   how often the sampler reads every source.
///   roots            where the machine's files are: the running machine's.
///   host             what the machine's registers say that no file does.
///   sink             where the result's bytes go; null is the filesystem.
///   sample           false runs the supervisor alone: no source is probed, no
///                    round taken. The command line never says it; the tests
///                    of the memory budget and the OOM record, which script
///                    the seam's reads in the order those readers make them,
///                    do.
///   sources, summarizer   how the sources are made, and what summarises the
///                    timeline; the defaults are the real ones (make_sources,
///                    CompensatedSummarizer). A test names others to reach
///                    what the real ones never do: a probe the timeline
///                    refuses, a window the summarizer refuses.
///
/// What the workers verify (M2):
///   capabilities     for a run that does not sample: whether the telemetry
///                    sources are probed, once, for the banner. A run that
///                    samples states the report of the probe its sampler made
///                    and no other (a source is probed once). Always, for a
///                    real run; a test that runs over a fake of the system
///                    calls turns it off, because a fake that answers every
///                    path with the same file gives the hwmon scan (which
///                    walks every chip number) thousands of chips to read.
///   isa              the ISA path asked for; none: the best the machine and
///                    each workload offer.
///   seed             the run's seed; worker i runs seed + i * controller::kSeedStride.
///   footprint_bytes  a worker's footprint when its configuration names none.
///   isolation_limit  the most wrong results of one worker that fault isolation repeats.
///   lock             what each worker's runner does about locking its working set
///                    (verification::LockPolicy); `stress --test --lock` sets it.
///
/// What the workers are told while they run, and the crash journal (M5):
///   schedule         the flagship mixed workload's rotations or thermal and
///                    power cycling's levels (workloads/mixed): carried out by
///                    the controller. Not owned. Null: the workers run what
///                    they were started as. `stress --test mixed` and
///                    `stress --test cycling` make one.
///   journal_path     the crash journal (main/journal_rig.hpp): empty, none is
///                    written. `--journal FILE` sets it.
///   journal_ops, journal_files   how the journal reaches the filesystem; null
///                    is the real one. A test names a double that fails.
struct StressOptions {
  std::vector<std::string> formats;
  std::string output_dir = ".";
  std::int64_t sample_interval_ns = 1'000'000'000;
  MachineRoots roots;
  std::optional<HostFacts> host;  ///< Null: read_host_facts().
  ResultSink* sink = nullptr;
  bool sample = true;
  SourceFactory sources = make_sources;
  const statistics::Summarizer* summarizer = nullptr;

  bool capabilities = true;
  std::optional<simd::IsaPath> isa;
  std::uint64_t seed = 1;
  std::uint64_t footprint_bytes = std::uint64_t{32} << 20U;
  std::size_t isolation_limit = 16;
  verification::LockPolicy lock = verification::LockPolicy::kTry;
  const mixed::Schedule* schedule = nullptr;
  std::string journal_path;
  journal::FileOps* journal_ops = nullptr;
  JournalFiles* journal_files = nullptr;
};

/// The stress command's arguments after "stress": `--config FILE`, and
/// optionally `--output FORMATS`, `--output-dir DIR`, `--sample-interval
/// DURATION` and `--journal FILE`, in any order, each once. (`--test NAME` is
/// stress_test's.)
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

/// The stress command for one named workload: `loadforge stress --test NAME
/// [options]`. `args` starts at NAME. A workload this build does not have, or
/// an option misused, is a usage error that says which. One worker per CPU the
/// process may use, unless told otherwise, each pinned to its own. It takes the
/// options of `--output`, `--output-dir`, `--sample-interval` and `--journal` as
/// stress does; those it is given override the ones in `base`.
///
/// Two names are not workloads but runs made of a schedule (M5): `mixed`, the
/// flagship, in which the workers' workloads rotate while they go on verifying
/// (`--rotation DURATION` between rotations), and `cycling`, thermal and power
/// cycling of one workload (`--workload NAME`, `--cycle load|cores`, `--dwell
/// DURATION` per level).
[[nodiscard]] int stress_test(std::span<const std::string_view> args, const std::string& self_link,
                              std::string_view sysfs_root, const StressOptions& base,
                              std::ostream& out, std::ostream& err);

/// `loadforge journal FILE`: the crash journal at FILE, every entry in words and
/// how it ended. Exit 0 whatever the ending (an abnormal ending is what it is
/// there to show), 1 when there is no journal or it cannot be read. The
/// operations are named so that a test can fail them.
[[nodiscard]] int show_journal(const std::string& path, journal::FileOps& ops, JournalFiles& files,
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
