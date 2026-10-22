// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "controller/isolate.hpp"
#include "controller/journal/file_ops.hpp"
#include "controller/orchestration.hpp"
#include "controller/rerun.hpp"
#include "controller/results/fingerprint.hpp"
#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "core/parse_error.hpp"
#include "core/parse_number.hpp"
#include "main/journal_rig.hpp"
#include "main/outcome.hpp"
#include "main/run_result.hpp"
#include "main/schedules.hpp"
#include "main/selftest.hpp"
#include "platform/affinity.hpp"
#include "platform/fpenv/real_control.hpp"
#include "platform/fs.hpp"
#include "platform/isa/probe.hpp"
#include "platform/process.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "reporters/console.hpp"
#include "resources/cgroup.hpp"
#include "resources/cgroup_oom_record.hpp"
#include "resources/memory_budget.hpp"
#include "statistics/summarizer.hpp"
#include "telemetry/capability/capability.hpp"
#include "topology/cache.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/snapshot.hpp"
#include "verification/isolation/ladder.hpp"
#include "worker/entry.hpp"
#include "worker/services.hpp"
#include "workloads/mixed/cycling.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::cli {
namespace {

constexpr int kOk = 0;
constexpr int kConfigRefused = 1;  ///< Also: a run that could not start.
constexpr int kUsageError = 2;

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
/// How long a worker under a schedule has to finish its unit and leave once asked.
constexpr std::int64_t kScheduledStopGraceNs = 5'000'000'000;

/// The longest a run or an interval may be taken to be in nanoseconds-safe
/// milliseconds: a quarter of what a signed 64-bit count of nanoseconds holds,
/// about 73 years, so that sums of a few such lengths do not overflow.
constexpr std::int64_t kLongestMilliseconds =
    std::numeric_limits<std::int64_t>::max() / kNanosecondsPerMillisecond / 4;

/// How long a `--test` runs when it is not told.
constexpr std::int64_t kDefaultTestMilliseconds = 10'000;
/// The most workers a `--test` starts when it is not told how many: one per
/// CPU the process may use, up to this.
constexpr std::size_t kMostDefaultWorkers = 64;
/// How stale a verifying worker's beat may be before it is called a hang. A
/// slice is never shorter than one unit and a unit has no upper bound in the
/// contracts (a worker cannot beat from inside compute()), so a controller that
/// called every worker whose progress stood still for the M1 allowance of two
/// seconds a hang would kill the healthy ones on a slow build or a loaded
/// machine: a sort unit takes 0.4 s in a debug build and ten times that under a
/// sanitizer (journal fragment 11). Ten seconds; null workloads keep their two.
constexpr std::int64_t kVerifyingStaleAfterNs = 10'000'000'000;

void print_usage(std::ostream& out) {
  out << "loadforge " << kVersion << "\n"
      << "\n"
      << "Usage:\n"
      << "  loadforge --version\n"
      << "  loadforge --help\n"
      << "  loadforge --check-config FILE   parse and validate a configuration\n"
      << "  loadforge stress --config FILE [--output FORMATS] [--output-dir DIR]\n"
      << "                   [--sample-interval DURATION] [--journal FILE]\n"
      << "                                  run a configuration under supervision,\n"
      << "                                  sampling every telemetry source; FORMATS is\n"
      << "                                  json, csv or json,csv, written to DIR/run.<format>\n"
      << "  loadforge stress --test NAME [--duration D] [--workers N] [--footprint SIZE]\n"
      << "                   [--isa scalar|avx2|avx512|neon] [--seed N]\n"
      << "                   [--lock require|try|none] [--journal FILE]\n"
      << "                   [--output FORMATS] [--output-dir DIR] [--sample-interval DURATION]\n"
      << "                                  stress with one verified workload, sampled and\n"
      << "                                  written the same way; NAME is one of\n"
      << "                                  adaptive_mesh, compression, dense_linear, fft, graph,\n"
      << "                                  monte_carlo, particles, raytrace, sort, sparse,\n"
      << "                                  stencil, or one of the two runs made of a schedule:\n"
      << "                                  mixed [--rotation D]   the flagship: workloads rotate\n"
      << "                                  across the workers, and the workers across the CPUs,\n"
      << "                                  while every unit goes on being verified\n"
      << "                                  cycling [--workload NAME] [--cycle load|cores]\n"
      << "                                  [--dwell D]   thermal and power cycling\n"
      << "  loadforge journal FILE          print a crash journal and say how it ended\n"
      << "\n"
      << "With --journal, a run writes a crash journal to FILE (an entry every few seconds,\n"
      << "each synced to disk) and, at the next start, says how the previous one ended and\n"
      << "keeps it as FILE.1, FILE.2 ... if it did not end cleanly.\n"
      << "\n"
      << "The configured run.duration is the time the workers run, counted from the moment\n"
      << "the last of them has been started; starting them is not part of it, and a run on a\n"
      << "slow or loaded machine takes that much longer in all.\n"
      << "\n"
      << "Exit codes: 0 clean; 1 refused (configuration, or the run could not start);\n"
      << "2 usage; 3 the run ended with a worker that did not complete; 4 a result file\n"
      << "could not be written; 5 the run found a fault (a wrong result, or an error the\n"
      << "hardware reported, corrected or not); 6 a check was not performed (a run that\n"
      << "checked nothing is not clean). When several apply the first of 5, 4, 3, 6 is the\n"
      << "code.\n"
      << "See docs/PLAN.md for what lands when.\n";
}

/// What `stress --test` was asked for.
struct TestArgs {
  std::string workload;
  std::optional<core::Duration> duration;
  std::optional<std::uint64_t> workers;
  std::optional<core::ByteSize> footprint;
  std::optional<simd::IsaPath> isa;
  std::optional<std::uint64_t> seed;
  std::optional<verification::LockPolicy> lock;
  std::vector<std::string> formats;  ///< --output: none given is none written.
  std::optional<std::string> output_dir;
  std::optional<std::int64_t> sample_interval_ns;
  std::optional<std::string> journal;
  std::optional<core::Duration> rotation;  ///< mixed only.
  std::optional<core::Duration> dwell;     ///< cycling only.
  std::optional<mixed::CycleKind> cycle;   ///< cycling only.
  std::optional<std::string> on_workload;  ///< cycling only.
};

/// The sample interval a flag names, in nanoseconds, or what is wrong with it.
core::Result<std::int64_t, std::string> sample_interval_of(std::string_view text) {
  auto duration = core::parse_duration(text);
  if (!duration.has_value()) {
    return "--sample-interval: " + std::string{core::describe(duration.error())};
  }
  if (duration.value().count() <= 0) {
    return std::string{"--sample-interval must be above zero"};
  }
  if (duration.value().count() > kLongestMilliseconds) {
    return std::string{"--sample-interval is longer than any run can be"};
  }
  return duration.value().count() * kNanosecondsPerMillisecond;
}

/// "key: what" -- the shape of every refusal of an option, naming it.
std::string refused(std::string_view key, const std::string& what) {
  std::string text{key};
  text += ": ";
  text += what;
  return text;
}

core::Result<std::uint64_t, std::string> whole_number(std::string_view key, std::string_view text) {
  auto split = core::split_number_and_suffix(text);
  if (!split.has_value() || !split.value().suffix.empty()) {
    return refused(key, "expected a whole number, got '" + std::string{text} + "'");
  }
  return split.value().value;
}

/// An option as it was written: its key and the value after it. One argument
/// where two string views would be swapped by mistake.
struct Written {
  std::string_view key;
  std::string_view value;
};

std::optional<std::string> take_duration(const Written& option, TestArgs& out) {
  auto duration = core::parse_duration(option.value);
  if (!duration.has_value()) {
    return refused(option.key, std::string{core::describe(duration.error())});
  }
  out.duration = duration.value();
  return std::nullopt;
}

std::optional<std::string> take_workers(const Written& option, TestArgs& out) {
  auto count = whole_number(option.key, option.value);
  if (!count.has_value()) {
    return count.error();
  }
  if (count.value() == 0) {
    return refused(option.key, "must be at least 1");
  }
  out.workers = count.value();
  return std::nullopt;
}

std::optional<std::string> take_footprint(const Written& option, TestArgs& out) {
  auto size = core::parse_byte_size(option.value);
  if (!size.has_value()) {
    return refused(option.key, std::string{core::describe(size.error())});
  }
  out.footprint = size.value();
  return std::nullopt;
}

std::optional<std::string> take_isa(const Written& option, TestArgs& out) {
  out.isa = simd::parse_isa_path(option.value);
  if (!out.isa.has_value()) {
    return refused(option.key,
                   "'" + std::string{option.value} + "' is none of scalar, avx2, avx512, neon");
  }
  return std::nullopt;
}

std::optional<std::string> take_lock(const Written& option, TestArgs& out) {
  for (const verification::LockPolicy policy :
       {verification::LockPolicy::kRequire, verification::LockPolicy::kTry,
        verification::LockPolicy::kNone}) {
    if (verification::describe(policy) == option.value) {
      out.lock = policy;
      return std::nullopt;
    }
  }
  return refused(option.key, "'" + std::string{option.value} + "' is none of require, try, none");
}

std::optional<std::string> take_seed(const Written& option, TestArgs& out) {
  auto seed = whole_number(option.key, option.value);
  if (!seed.has_value()) {
    return seed.error();
  }
  out.seed = seed.value();
  return std::nullopt;
}

std::optional<std::string> take_output(const Written& option, TestArgs& out) {
  auto list = parse_formats(option.value);
  if (!list.has_value()) {
    return list.error();
  }
  out.formats = std::move(list).value();
  return std::nullopt;
}

std::optional<std::string> take_output_dir(const Written& option, TestArgs& out) {
  out.output_dir = std::string{option.value};
  return std::nullopt;
}

std::optional<std::string> take_sample_interval(const Written& option, TestArgs& out) {
  auto interval = sample_interval_of(option.value);
  if (!interval.has_value()) {
    return interval.error();
  }
  out.sample_interval_ns = interval.value();
  return std::nullopt;
}

std::optional<std::string> take_journal(const Written& option, TestArgs& out) {
  out.journal = std::string{option.value};
  return std::nullopt;
}

/// A duration that is above zero and that a run can have.
core::Result<core::Duration, std::string> positive_duration(const Written& option) {
  auto duration = core::parse_duration(option.value);
  if (!duration.has_value()) {
    return refused(option.key, std::string{core::describe(duration.error())});
  }
  if (duration.value().count() <= 0 || duration.value().count() > kLongestMilliseconds) {
    return refused(option.key, "must be above zero and no longer than a run can be");
  }
  return duration.value();
}

std::optional<std::string> take_rotation(const Written& option, TestArgs& out) {
  auto duration = positive_duration(option);
  if (!duration.has_value()) {
    return duration.error();
  }
  out.rotation = duration.value();
  return std::nullopt;
}

std::optional<std::string> take_dwell(const Written& option, TestArgs& out) {
  auto duration = positive_duration(option);
  if (!duration.has_value()) {
    return duration.error();
  }
  out.dwell = duration.value();
  return std::nullopt;
}

std::optional<std::string> take_cycle(const Written& option, TestArgs& out) {
  for (const mixed::CycleKind kind : {mixed::CycleKind::kLoad, mixed::CycleKind::kCores}) {
    if (option.value == mixed::describe(kind)) {
      out.cycle = kind;
      return std::nullopt;
    }
  }
  return refused(option.key, "'" + std::string{option.value} + "' is none of load, cores");
}

std::optional<std::string> take_on_workload(const Written& option, TestArgs& out) {
  out.on_workload = std::string{option.value};
  return std::nullopt;
}

/// Each option of `stress --test`: how it is read, and whether it has been.
struct Option {
  std::string_view key;
  std::optional<std::string> (*take)(const Written&, TestArgs&);
  bool (*given)(const TestArgs&);
};

constexpr std::array<Option, 14> kOptions{{
    {"--duration", &take_duration, [](const TestArgs& a) { return a.duration.has_value(); }},
    {"--workers", &take_workers, [](const TestArgs& a) { return a.workers.has_value(); }},
    {"--footprint", &take_footprint, [](const TestArgs& a) { return a.footprint.has_value(); }},
    {"--isa", &take_isa, [](const TestArgs& a) { return a.isa.has_value(); }},
    {"--seed", &take_seed, [](const TestArgs& a) { return a.seed.has_value(); }},
    {"--lock", &take_lock, [](const TestArgs& a) { return a.lock.has_value(); }},
    {"--output", &take_output, [](const TestArgs& a) { return !a.formats.empty(); }},
    {"--output-dir", &take_output_dir, [](const TestArgs& a) { return a.output_dir.has_value(); }},
    {"--sample-interval", &take_sample_interval,
     [](const TestArgs& a) { return a.sample_interval_ns.has_value(); }},
    {"--journal", &take_journal, [](const TestArgs& a) { return a.journal.has_value(); }},
    {"--rotation", &take_rotation, [](const TestArgs& a) { return a.rotation.has_value(); }},
    {"--dwell", &take_dwell, [](const TestArgs& a) { return a.dwell.has_value(); }},
    {"--cycle", &take_cycle, [](const TestArgs& a) { return a.cycle.has_value(); }},
    {"--workload", &take_on_workload, [](const TestArgs& a) { return a.on_workload.has_value(); }},
}};

/// The options after `--test`: its name, then any of the options in any order,
/// each once. A refusal names the option.
core::Result<TestArgs, std::string> parse_test_args(std::span<const std::string_view> args) {
  TestArgs parsed;
  parsed.workload = std::string{args.front()};
  for (std::size_t index = 1; index < args.size(); index += 2) {
    const std::string_view key = args[index];
    if (index + 1 >= args.size()) {
      return refused(key, "missing its value");
    }
    const auto* const option = std::ranges::find(kOptions, key, &Option::key);
    if (option == kOptions.end() || option->given(parsed)) {
      return refused(key, "not an option of stress --test, or given twice");
    }
    if (auto refusal = option->take(Written{key, args[index + 1]}, parsed); refusal.has_value()) {
      return *refusal;
    }
  }
  return parsed;
}

/// The cache-line size the machine has, from its caches: the largest any of
/// them reports (they agree wherever the hardware does), or 0 when none
/// reports one. A classification by an assumed 64 would be a fabricated fact.
std::uint32_t line_bytes_of(const topology::Snapshot& machine) {
  std::uint32_t line = 0;
  for (const topology::Cache& cache : machine.caches.caches()) {
    line = std::max(line, cache.line_bytes.value_or(0));
  }
  return line;
}

/// The CPUs a worker may be placed on: those the process is allowed to run on
/// that the snapshot found, ascending. When the kernel will not say what is
/// allowed, the snapshot's CPUs: placement then fails per worker, by name, if
/// one is withheld.
std::vector<std::uint32_t> allowed_cpus(platform::Syscalls& syscalls,
                                        const topology::Snapshot& machine) {
  std::vector<std::uint32_t> found;
  for (const topology::LogicalCpu& cpu : machine.cpus.cpus()) {
    found.push_back(cpu.id);
  }
  platform::Affinity affinity(syscalls);
  auto permitted = affinity.permitted();
  if (!permitted.has_value()) {
    return found;
  }
  const std::vector<std::uint32_t> allowed = platform::ids_of(permitted.value());
  std::erase_if(found, [&allowed](std::uint32_t id) {
    return std::ranges::find(allowed, id) == allowed.end();
  });
  return found;
}

/// The executable and the machine: what every run needs before its first
/// fork, or the refusal.
struct Launch {
  std::string executable;
  topology::Snapshot machine;
};

std::optional<Launch> prepare_launch(platform::Syscalls& syscalls, const std::string& self_link,
                                     std::string_view sysfs_root, std::ostream& err) {
  platform::ExecutablePath self(syscalls);
  auto executable = self.resolve(self_link);
  if (!executable.has_value()) {
    err << "loadforge: cannot resolve this executable for the workers: "
        << describe(executable.error()) << "\n";
    return std::nullopt;
  }
  // The machine, described once and before any worker exists (docs/PLAN.md
  // §4.1): the frame every later report is attributed against. A machine
  // whose /sys contradicts itself is refused here rather than run on and
  // described wrongly.
  platform::FileSystem sysfs(syscalls);
  auto machine = topology::take_snapshot(sysfs, sysfs_root);
  if (!machine.has_value()) {
    err << "loadforge: cannot describe this machine before starting workers: "
        << topology::describe(machine.error()) << "\n";
    return std::nullopt;
  }
  return std::optional<Launch>{std::in_place, std::move(executable).value(),
                               std::move(machine).value()};
}

/// What `stress --test` is given beside its own options.
const StressOptions kDefaultOptions{};

}  // namespace

HostFacts read_host_facts() {
  const fpenv::RealFpControl control;
  // Never fails without a request: scalar is always eligible.
  const simd::IsaPath path =
      simd::select_path(std::nullopt, isa::make_native_probe()->supported(), simd::compiled_paths())
          .value();
  return HostFacts{control.read(), path};
}

namespace {

/// Where each flag's value goes, once. A flag given twice, or without a value,
/// is the caller's mistake and is said.
struct StressFlags {
  std::optional<std::string_view> config;
  std::optional<std::string_view> output;
  std::optional<std::string_view> output_dir;
  std::optional<std::string_view> interval;
  std::optional<std::string_view> journal;
};

/// The place a flag's value goes, or null for a flag stress does not take.
std::optional<std::string_view>* slot_of(StressFlags& flags, std::string_view flag) {
  if (flag == "--config") {
    return &flags.config;
  }
  if (flag == "--output") {
    return &flags.output;
  }
  if (flag == "--output-dir") {
    return &flags.output_dir;
  }
  if (flag == "--sample-interval") {
    return &flags.interval;
  }
  if (flag == "--journal") {
    return &flags.journal;
  }
  return nullptr;
}

core::Result<StressFlags, std::string> read_stress_flags(std::span<const std::string_view> args) {
  StressFlags flags;
  for (std::size_t at = 0; at < args.size(); at += 2) {
    std::optional<std::string_view>* slot = slot_of(flags, args[at]);
    if (slot == nullptr) {
      return "stress does not take " + std::string{args[at]};
    }
    if (at + 1 == args.size()) {
      return std::string{args[at]} + " takes a value";
    }
    if (slot->has_value()) {
      return std::string{args[at]} + " is given twice";
    }
    *slot = args[at + 1];
  }
  return flags;
}

}  // namespace

core::Result<StressArgs, std::string> parse_stress_args(std::span<const std::string_view> args) {
  auto read = read_stress_flags(args);
  if (!read.has_value()) {
    return read.error();
  }
  const StressFlags& flags = read.value();
  if (!flags.config.has_value()) {
    return std::string{"stress needs --config FILE"};
  }
  StressArgs parsed;
  parsed.config_path = std::string{*flags.config};
  if (flags.output_dir.has_value()) {
    parsed.options.output_dir = std::string{*flags.output_dir};
  }
  if (flags.output.has_value()) {
    auto list = parse_formats(*flags.output);
    if (!list.has_value()) {
      return list.error();
    }
    parsed.options.formats = std::move(list).value();
  }
  if (flags.interval.has_value()) {
    auto interval = sample_interval_of(*flags.interval);
    if (!interval.has_value()) {
      return interval.error();
    }
    parsed.options.sample_interval_ns = interval.value();
  }
  if (flags.journal.has_value()) {
    parsed.options.journal_path = std::string{*flags.journal};
  }
  return parsed;
}

int stress(std::string_view path, const std::string& self_link, std::string_view sysfs_root,
           const StressOptions& options, std::ostream& out, std::ostream& err) {
  const auto loaded = config::load(std::string{path});
  if (!loaded.has_value()) {
    err << "loadforge: " << config::describe(loaded.error()) << "\n";
    return kConfigRefused;
  }
  const config::Config& config = loaded.value();
  if (config.run.mode != config::Mode::kStress) {
    err << "loadforge: " << path << ": mode " << config::describe(config.run.mode)
        << " lands with a later milestone; stress is the one this build runs\n";
    return kConfigRefused;
  }
  platform::RealSyscalls syscalls;
  const std::optional<Launch> launch = prepare_launch(syscalls, self_link, sysfs_root, err);
  if (!launch.has_value()) {
    return kConfigRefused;
  }
  return run_stress(syscalls, config, launch->executable, launch->machine, options, out, err);
}

namespace {

/// What making a schedule needs that the request does not carry.
struct SchedulePlan {
  const workloads::Registry* registry;
  std::size_t workers;
  const std::vector<std::uint32_t>* cpus;
  std::uint64_t seed;
  core::Duration duration;
  std::uint64_t footprint_bytes;  ///< When the request names none.
};

/// The run `--test mixed` or `--test cycling` asked for, or why it cannot be had.
core::Result<ScheduledRun, std::string> plan_scheduled_run(const TestArgs& test,
                                                           const std::string& workload,
                                                           const SchedulePlan& plan) {
  if (test.workload == "mixed") {
    MixedRequest request;
    request.registry = plan.registry;
    request.workers = plan.workers;
    request.cpus = *plan.cpus;
    request.seed = plan.seed;
    request.duration_ms = plan.duration.count();
    if (test.rotation.has_value()) {
      request.rotation_ms = test.rotation->count();
    }
    request.footprint_bytes = plan.footprint_bytes;
    if (test.footprint.has_value()) {
      if (!test.footprint->is_absolute()) {
        return std::string{"--footprint must be a size, not a share of memory"};
      }
      request.footprint_bytes = test.footprint->byte_count();
    }
    return plan_mixed(request);
  }
  CyclingRequest request;
  request.workload = workload;
  request.kind = test.cycle.value_or(mixed::CycleKind::kLoad);
  request.workers = plan.workers;
  request.duration_ms = plan.duration.count();
  if (test.dwell.has_value()) {
    request.dwell_ms = test.dwell->count();
  }
  request.footprint = test.footprint;
  return plan_cycling(request);
}

}  // namespace

namespace {

/// The option of `stress --test` that does not belong to the run asked for, or
/// nothing: the schedules' options mean nothing to a plain workload, and a
/// mixed run takes no cycle's.
std::optional<std::string_view> misplaced_option(const TestArgs& test) {
  const bool mixed_run = test.workload == "mixed";
  const bool cycling_run = test.workload == "cycling";
  if (!mixed_run && test.rotation.has_value()) {
    return "--rotation";
  }
  if (!cycling_run && test.dwell.has_value()) {
    return "--dwell";
  }
  if (!cycling_run && test.cycle.has_value()) {
    return "--cycle";
  }
  if (!cycling_run && test.on_workload.has_value()) {
    return "--workload";
  }
  return std::nullopt;
}

}  // namespace

int stress_test(std::span<const std::string_view> args, const std::string& self_link,
                std::string_view sysfs_root, const StressOptions& base, std::ostream& out,
                std::ostream& err) {
  auto parsed = parse_test_args(args);
  if (!parsed.has_value()) {
    err << "loadforge: stress --test: " << parsed.error() << "\n";
    return kUsageError;
  }
  const TestArgs& test = parsed.value();
  if (const auto misplaced = misplaced_option(test); misplaced.has_value()) {
    err << "loadforge: stress --test " << test.workload << ": " << *misplaced
        << " is not an option of this run\n";
    return kUsageError;
  }
  const bool mixed_run = test.workload == "mixed";
  const bool cycling_run = test.workload == "cycling";
  const workloads::BuiltinRegistry registry;
  const std::vector<std::string> names = registry.names();
  // The workload the workers run, when one workload is what they run.
  const std::string workload =
      cycling_run ? test.on_workload.value_or("dense_linear") : test.workload;
  if (!mixed_run && std::ranges::find(names, workload) == names.end()) {
    err << "loadforge: stress --test: no workload named '" << workload << "'; this build has:";
    for (const std::string& name : names) {
      err << " " << name;
    }
    err << "\n";
    return kUsageError;
  }
  platform::RealSyscalls syscalls;
  const std::optional<Launch> launch = prepare_launch(syscalls, self_link, sysfs_root, err);
  if (!launch.has_value()) {
    return kConfigRefused;
  }
  const std::vector<std::uint32_t> cpus = allowed_cpus(syscalls, launch->machine);
  const core::Duration duration = test.duration.value_or(core::Duration{kDefaultTestMilliseconds});
  // One worker per CPU the process may use, each pinned to its own: the
  // placement a finding needs to be repeated on another core.
  const std::size_t workers = test.workers.has_value()
                                  ? static_cast<std::size_t>(*test.workers)
                                  : std::clamp<std::size_t>(cpus.size(), 1, kMostDefaultWorkers);
  StressOptions options = base;
  options.isa = test.isa;
  options.seed = test.seed.value_or(options.seed);
  options.lock = test.lock.value_or(options.lock);
  if (test.output_dir.has_value()) {
    options.output_dir = *test.output_dir;
  }
  if (!test.formats.empty()) {
    options.formats = test.formats;
  }
  if (test.journal.has_value()) {
    options.journal_path = *test.journal;
  }
  options.sample_interval_ns = test.sample_interval_ns.value_or(options.sample_interval_ns);

  // The run made of a schedule, held here to the end of the run.
  std::optional<ScheduledRun> scheduled;
  config::Config config;
  if (mixed_run || cycling_run) {
    const SchedulePlan plan{&registry,    workers,  &cpus,
                            options.seed, duration, options.footprint_bytes};
    auto planned = plan_scheduled_run(test, workload, plan);
    if (!planned.has_value()) {
      err << "loadforge: stress --test " << test.workload << ": " << planned.error() << "\n";
      return kConfigRefused;
    }
    scheduled = std::move(planned).value();
    config = scheduled->config;
    options.schedule = &scheduled->schedule;
  } else {
    config.run.mode = config::Mode::kStress;
    config.run.duration = duration;
    for (std::size_t index = 0; index < workers; ++index) {
      config.workloads.emplace_back(workload, config::Threads::count(1), true, test.footprint);
    }
  }
  return run_stress(syscalls, config, launch->executable, launch->machine, options, out, err);
}

namespace {

/// Everything a run's ending needs from the run.
struct Ending {
  const config::Config* config = nullptr;
  const controller::RunReport* report = nullptr;
  platform::FileSystem* machine = nullptr;
  const StressOptions* options = nullptr;
  const Rig* rig = nullptr;  ///< Null when nothing was sampled.
  const telemetry::TimelineView* timeline = nullptr;
  ResultSink* sink = nullptr;
};

/// Says that the run ended, in the journal: the terminator a replay reads as a
/// clean end. Whether the run did end cleanly is not asked here: the run came
/// back to say how it went, which is all a terminator claims -- a machine that
/// dies, or a controller that is killed, never gets here.
void end_journal(std::optional<RunJournal>& journal, std::ostream& err) {
  if (!journal.has_value()) {
    return;
  }
  if (auto closed = journal->writer->close(); !closed.has_value()) {
    err << "loadforge: the journal could not be closed, and will read as ended abnormally: "
        << closed.error().detail << "\n";
  }
}

/// The result of a run, said and written (M3), and the exit code it comes to.
int conclude_run(const Ending& ending, std::ostream& out, std::ostream& err) {
  const StressOptions& stress_options = *ending.options;
  const controller::RunReport& report = *ending.report;
  const HostFacts host = stress_options.host.has_value() ? *stress_options.host : read_host_facts();
  const statistics::CompensatedSummarizer real_summarizer;
  const statistics::Summarizer& summarizer =
      stress_options.summarizer != nullptr ? *stress_options.summarizer : real_summarizer;
  results::FingerprintContext context;
  context.build = results::this_build();
  context.fp_environment = host.fp_environment;
  context.isa_path = host.isa_path;
  auto reading = results::read_fingerprint(*ending.machine, "", context);
  ResultInputs inputs;
  inputs.config = ending.config;
  inputs.report = &report;
  inputs.fingerprint = reading.fingerprint;
  if (ending.rig != nullptr) {
    inputs.capabilities = ending.rig->capabilities();
  }
  inputs.timeline = ending.timeline;
  inputs.summarizer = &summarizer;
  auto result = assemble_result(inputs);
  if (!result.has_value()) {
    err << "loadforge: cannot compute the result: " << result.error() << "\n";
    return with_output_failure(exit_code_for(report, *ending.config));
  }
  out << summary_text(result.value(), report, *ending.timeline, summarizer, reading.gaps);
  const int code = exit_code_for(result.value().verdict, controller::all_clean(report),
                                 has_verified_worker(report));
  if (stress_options.formats.empty()) {
    return code;
  }
  auto written = write_results(result.value(), stress_options.formats, stress_options.output_dir,
                               *ending.sink);
  if (!written.has_value()) {
    err << "loadforge: cannot write the result: " << written.error() << "\n";
    return with_output_failure(code);
  }
  for (const std::string& file : written.value()) {
    out << "result written: " << file << "\n";
  }
  return code;
}

/// The journal this run was asked to keep, opened, and what the previous one at the
/// path said; none when the run was not asked to keep one. The operations are the
/// options' own when a test gave them, else the filesystem's.
core::Result<std::optional<RunJournal>, std::string> begin_journal(
    const StressOptions& stress_options, journal::FileOps& real_ops, JournalFiles& real_files,
    std::ostream& out) {
  if (stress_options.journal_path.empty()) {
    return std::optional<RunJournal>{};
  }
  journal::FileOps& ops =
      stress_options.journal_ops != nullptr ? *stress_options.journal_ops : real_ops;
  JournalFiles& housekeeping =
      stress_options.journal_files != nullptr ? *stress_options.journal_files : real_files;
  auto opened = open_run_journal(ops, housekeeping, stress_options.journal_path);
  if (!opened.has_value()) {
    return opened.error();
  }
  std::optional<RunJournal> journal_open = std::move(opened).value();
  if (!journal_open->previous.empty()) {
    out << journal_open->previous << "\n";
  }
  return journal_open;
}

}  // namespace

int run_stress(platform::Syscalls& syscalls, const config::Config& config,
               const std::string& executable, const topology::Snapshot& snapshot,
               const StressOptions& stress_options, std::ostream& out, std::ostream& err) {
  controller::RunOptions options;
  options.executable = executable;
  options.topology = &snapshot;
  // What the machine offers and what the kernel will say about an OOM kill,
  // both read through the same seam the run uses: the running machine's
  // files on the real one.
  platform::FileSystem machine(syscalls);
  auto budget = resources::read_memory_budget(machine, resources::kMachineRoot);
  if (budget.has_value()) {
    options.budget = budget.value();
  } else {
    options.budget_unread = topology::describe(budget.error());
  }
  resources::CgroupOomKillRecord oom_record(machine, resources::kMachineRoot);
  options.oom_record = &oom_record;

  // The output is checked before the run, not after it.
  FileResultSink files;
  ResultSink& sink = stress_options.sink != nullptr ? *stress_options.sink : files;
  if (!stress_options.formats.empty()) {
    if (auto prepared = sink.prepare(stress_options.output_dir); !prepared.has_value()) {
      err << "loadforge: cannot write the result: " << prepared.error() << "\n";
      return kConfigRefused;
    }
  }

  // The crash journal: what the previous run left at the path is replayed, said
  // and kept before anything is started, and a run asked to journal that cannot
  // is refused, not run without the evidence.
  journal::RealFileOps real_ops;
  RealJournalFiles real_files;
  auto begun = begin_journal(stress_options, real_ops, real_files, out);
  if (!begun.has_value()) {
    err << "loadforge: " << begun.error() << "\n";
    return kConfigRefused;
  }
  std::optional<RunJournal> journal_open = std::move(begun).value();
  if (journal_open.has_value()) {
    options.journal = journal_open->writer.get();
  }
  options.schedule = stress_options.schedule;
  if (options.schedule != nullptr) {
    // A schedule moves a worker between workloads whose units differ widely in
    // length, and a worker is only asked to stop between two units: a stop
    // grace long enough for one workload's unit would kill a worker in the
    // middle of another's.
    options.grace.stop_ns = kScheduledStopGraceNs;
  }

  // The verified runs: the workloads this build has, the ISA path chosen once
  // from what this CPU can run, and the floating-point registers established
  // and asserted before anything starts (M2a exit criteria 1 and 5; M2b 4).
  const workloads::BuiltinRegistry registry;
  const std::unique_ptr<simd::IsaProbe> probe = isa::make_native_probe();
  fpenv::RealFpControl fp;
  const std::vector<std::string> registered = registry.names();
  const bool verifying =
      std::ranges::any_of(config.workloads, [&registered](const config::Workload& w) {
        return w.enabled && std::ranges::find(registered, w.name) != registered.end();
      });
  if (verifying) {
    options.stale_after_ns = std::max(options.stale_after_ns, kVerifyingStaleAfterNs);
  }
  options.verification.registry = &registry;
  options.verification.seed = stress_options.seed;
  options.verification.footprint_bytes = stress_options.footprint_bytes;
  options.verification.requested_isa = stress_options.isa;
  options.verification.lock = stress_options.lock;
  options.verification.isa_probe = probe.get();
  options.verification.line_bytes = line_bytes_of(snapshot);
  options.verification.cpus = allowed_cpus(syscalls, snapshot);
  options.fp = &fp;

  // The telemetry, probed before the first fork: what the machine offers is
  // stated first, with the remedy for each source that cannot be read, and the
  // sources that hold descriptors hold them before any worker is born. The
  // sources are probed once, by the rig that samples them (telemetry/source.hpp:
  // "called once, before any sample()"), so the controller is not also handed
  // sources to probe for its banner: that is for a run that samples nothing.
  options.sample_interval_ns = stress_options.sample_interval_ns;
  std::unique_ptr<Rig> rig;
  std::unique_ptr<Rig> probe_only;
  if (stress_options.sample) {
    const std::int64_t run_ms = std::min(config.run.duration.count(), kLongestMilliseconds);
    const std::int64_t run_ns = (run_ms * kNanosecondsPerMillisecond) + options.grace.stop_ns +
                                options.grace.kill_ns + options.stale_after_ns;
    const RunShape shape{run_ns, options.sample_interval_ns,
                         series_bound(snapshot.cpus.logical_cpu_count())};
    const std::size_t capacity = timeline_capacity(shape);
    auto made = make_rig(syscalls, machine, stress_options.roots, capacity, stress_options.sources);
    if (!made.has_value()) {
      err << "loadforge: cannot set up telemetry: " << telemetry::describe(made.error().fault)
          << ": " << made.error().detail << "\n";
      return kConfigRefused;
    }
    rig = std::move(made).value();
    options.sampler = &rig->sampler();
    options.timeline = &rig->timeline();
    out << telemetry::render_capabilities(rig->capabilities());
  } else if (stress_options.capabilities) {
    // A run that samples nothing still states what the machine offers, from a
    // sampler's probe like every other path: a rig that is probed, rendered
    // into the banner and never sampled. It lives to the end of the run, so the
    // sources hold their descriptors before any worker is born.
    auto made = make_rig(syscalls, machine, stress_options.roots,
                         timeline_capacity({0, options.sample_interval_ns,
                                            series_bound(snapshot.cpus.logical_cpu_count())}),
                         stress_options.sources);
    if (!made.has_value()) {
      err << "loadforge: cannot set up telemetry: " << telemetry::describe(made.error().fault)
          << ": " << made.error().detail << "\n";
      return kConfigRefused;
    }
    probe_only = std::move(made).value();
    options.capabilities = telemetry::render_capabilities(probe_only->capabilities());
  }
  const telemetry::BoundedTimeline no_timeline(0);
  const telemetry::BoundedTimeline& timeline = rig != nullptr ? rig->timeline() : no_timeline;
  TimelineNotes journal_notes(timeline);
  options.journal_notes = &journal_notes;

  reporters::ConsoleReporter reporter(out);
  controller::Controller supervisor(syscalls, reporter);
  // Flushed before the controller forks, so a child that fails to exec and
  // exits normally cannot replay buffered output.
  out.flush();
  err.flush();
  auto ran = supervisor.run(config, options);
  if (!ran.has_value()) {
    end_journal(journal_open, err);
    err << "loadforge: " << describe(ran.error()) << "\n";
    return kConfigRefused;
  }
  controller::RunReport report = std::move(ran).value();
  const std::optional<int> child_code = report.worker_exit_code;
  if (child_code.has_value()) {
    return *child_code;  // This process is a worker that could not exec.
  }
  end_journal(journal_open, err);
  // The workers are gone: what is left to do is repeat the units that came
  // out wrong, in processes of their own and say what that shows (M2); then
  // put the result together, say it, and write it (M3). A re-run that could
  // not exec is this process, and ends it.
  verification::isolation::LadderIsolator isolator;
  controller::RealRerunner rerunner(syscalls, executable, controller::RerunOptions{});
  const Ending ending{&config, &report, &machine, &stress_options, rig.get(), &timeline, &sink};
  const auto conclude = [&ending, &out, &err] { return conclude_run(ending, out, err); };
  return after_isolation(report, &snapshot, isolator, rerunner, stress_options.isolation_limit,
                         reporter, conclude);
}

// The two streams are the command's standard output and error, in the order every command
// of this file takes them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int show_journal(const std::string& path, journal::FileOps& ops, JournalFiles& files,
                 std::ostream& out, std::ostream& err) {
  auto shown = read_journal(ops, files, path);
  if (!shown.has_value()) {
    err << "loadforge: " << shown.error() << "\n";
    return kConfigRefused;
  }
  out << render_replay(shown.value(), path, true);
  return kOk;
}

namespace {

/// Reads a configuration and says whether it would be accepted, and why not
/// if not. Exit 1 on a refusal so a script can rely on it, and the message on
/// stderr where a script expects a diagnostic.
int check_config(std::string_view path, std::ostream& out, std::ostream& err) {
  const auto loaded = config::load(std::string{path});
  if (!loaded.has_value()) {
    err << "loadforge: " << config::describe(loaded.error()) << "\n";
    return kConfigRefused;
  }
  const config::Config& config = loaded.value();
  out << "ok: " << path << ": " << config::describe(config.run.mode) << " for "
      << core::to_string(config.run.duration) << ", " << config.workloads.size()
      << " workload(s)\n";
  return kOk;
}

}  // namespace

std::vector<std::string_view> collect_args(std::span<char* const> argv) {
  if (argv.empty()) {
    return {};
  }
  const std::span<char* const> without_program = argv.subspan(1);
  std::vector<std::string_view> args;
  args.reserve(without_program.size());
  for (const char* arg : without_program) {
    args.emplace_back(arg);
  }
  return args;
}

// bugprone-easily-swappable-parameters fires on the adjacent ostream
// references. (out, err) is the conventional shape for a testable CLI entry
// point, and contorting it to satisfy the check would make the code worse. This
// is suppressed here rather than project-wide so the check can still catch a
// genuinely dangerous pair elsewhere -- particularly in the platform and
// control code still to come.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (args.empty()) {
    print_usage(out);
    return kOk;
  }
  const std::string_view command = args.front();
  if (command == worker::kWorkerSwitch) {
    // The internal switch (docs/PLAN.md §4.6): this process is a worker the
    // controller re-executed. Real system calls, because there is nothing
    // else a worker could run on; the worker's own decisions are tested
    // through worker::run with a fake.
    platform::RealSyscalls syscalls;
    return worker::run(syscalls, args.subspan(1), err);
  }
  if (command == worker::kRerunSwitch) {
    // The isolation ladder's re-run (docs/verification.md §7): one unit, one
    // CPU, every check on, the answer in the exit code.
    platform::RealSyscalls syscalls;
    const worker::ProductionServices production;
    return worker::run_rerun(syscalls, args.subspan(1), err, production.services());
  }
  if (command == "--check-config") {
    if (args.size() != 2) {
      err << "loadforge: --check-config takes exactly one FILE\n";
      return kUsageError;
    }
    return check_config(args[1], out, err);
  }
  if (command == "stress") {
    if (args.size() >= 3 && args[1] == "--test") {
      return stress_test(args.subspan(2), "/proc/self/exe", topology::kDefaultSysfsRoot,
                         kDefaultOptions, out, err);
    }
    const auto parsed = parse_stress_args(args.subspan(1));
    if (!parsed.has_value()) {
      err << "loadforge: " << parsed.error() << "\n";
      return kUsageError;
    }
    return stress(parsed.value().config_path, "/proc/self/exe", topology::kDefaultSysfsRoot,
                  parsed.value().options, out, err);
  }
  if (command == "journal") {
    if (args.size() != 2) {
      err << "loadforge: journal takes exactly one FILE\n";
      return kUsageError;
    }
    journal::RealFileOps ops;
    RealJournalFiles files;
    return show_journal(std::string{args[1]}, ops, files, out, err);
  }
  if (args.size() > 1) {
    err << "loadforge: expected a single argument, got " << args.size() << "\n";
    return kUsageError;
  }

  if (command == "--version" || command == "-V") {
    out << kVersion << "\n";
    return kOk;
  }
  if (command == "--selftest-digest") {
    // Cross-architecture determinism check: the scalar build must produce an
    // identical digest on x86-64 and ARM64. See docs/determinism.md.
    out << selftest::core_digest_hex() << "\n";
    return kOk;
  }
  if (command == "--help" || command == "-h") {
    print_usage(out);
    return kOk;
  }

  err << "loadforge: unknown argument '" << command << "'\n"
      << "Try 'loadforge --help'.\n";
  return kUsageError;
}

}  // namespace loadforge::cli
