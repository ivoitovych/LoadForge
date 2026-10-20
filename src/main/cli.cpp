// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <algorithm>
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
#include "controller/orchestration.hpp"
#include "controller/results/fingerprint.hpp"
#include "core/duration.hpp"
#include "main/run_result.hpp"
#include "main/selftest.hpp"
#include "platform/fpenv/real_control.hpp"
#include "platform/fs.hpp"
#include "platform/isa/probe.hpp"
#include "platform/process.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "reporters/console.hpp"
#include "resources/cgroup.hpp"
#include "resources/cgroup_oom_record.hpp"
#include "resources/memory_budget.hpp"
#include "statistics/summarizer.hpp"
#include "telemetry/capability/capability.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/snapshot.hpp"
#include "worker/entry.hpp"

namespace loadforge::cli {
namespace {

constexpr int kOk = 0;
constexpr int kConfigRefused = 1;  ///< Also: a run that could not start.
constexpr int kUsageError = 2;
constexpr int kRunNotClean = 3;   ///< The run ended and at least one worker did not complete.
constexpr int kOutputFailed = 4;  ///< The run ended and a result file could not be written.
constexpr int kFaultsFound = 5;   ///< The run ended and found a fault: see exit_code_for.
constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
/// The longest a run or an interval may be taken to be in nanoseconds-safe
/// milliseconds: a quarter of what a signed 64-bit count of nanoseconds holds,
/// about 73 years, so that sums of a few such lengths do not overflow.
constexpr std::int64_t kLongestMilliseconds =
    std::numeric_limits<std::int64_t>::max() / kNanosecondsPerMillisecond / 4;

void print_usage(std::ostream& out) {
  out << "loadforge " << kVersion << "\n"
      << "\n"
      << "Usage:\n"
      << "  loadforge --version\n"
      << "  loadforge --help\n"
      << "  loadforge --check-config FILE   parse and validate a configuration\n"
      << "  loadforge stress --config FILE [--output FORMATS] [--output-dir DIR]\n"
      << "                   [--sample-interval DURATION]\n"
      << "                                  run the null workload under supervision,\n"
      << "                                  sampling every telemetry source; FORMATS is\n"
      << "                                  json, csv or json,csv, written to DIR/run.<format>\n"
      << "\n"
      << "Exit codes: 0 clean; 1 refused (configuration, or the run could not start);\n"
      << "2 usage; 3 the run ended with a worker that did not complete; 4 a result file\n"
      << "could not be written; 5 the run found a fault (a wrong result, or an error the\n"
      << "hardware reported, corrected or not).\n"
      << "See docs/PLAN.md for what lands when.\n";
}

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
    auto duration = core::parse_duration(*flags.interval);
    if (!duration.has_value()) {
      return "--sample-interval: " + std::string{core::describe(duration.error())};
    }
    if (duration.value().count() <= 0) {
      return std::string{"--sample-interval must be above zero"};
    }
    if (duration.value().count() > kLongestMilliseconds) {
      return std::string{"--sample-interval is longer than any run can be"};
    }
    parsed.options.sample_interval_ns = duration.value().count() * kNanosecondsPerMillisecond;
  }
  return parsed;
}

int exit_code_for(results::Verdict verdict, bool workers_clean) noexcept {
  if (verdict == results::Verdict::kFindings || verdict == results::Verdict::kCorrectedErrors) {
    return kFaultsFound;
  }
  return workers_clean ? kOk : kRunNotClean;
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
  platform::ExecutablePath self(syscalls);
  auto executable = self.resolve(self_link);
  if (!executable.has_value()) {
    err << "loadforge: cannot resolve this executable for the workers: "
        << describe(executable.error()) << "\n";
    return kConfigRefused;
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
    return kConfigRefused;
  }
  return run_stress(syscalls, config, executable.value(), machine.value(), options, out, err);
}

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

  // The telemetry, probed before the first fork: what the machine offers is
  // stated first, with the remedy for each source that cannot be read, and the
  // sources that hold descriptors hold them before any worker is born.
  options.sample_interval_ns = stress_options.sample_interval_ns;
  std::unique_ptr<Rig> rig;
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
  }
  const telemetry::BoundedTimeline no_timeline(0);
  const telemetry::BoundedTimeline& timeline = rig != nullptr ? rig->timeline() : no_timeline;

  reporters::ConsoleReporter reporter(out);
  controller::Controller supervisor(syscalls, reporter);
  // Flushed before the controller forks, so a child that fails to exec and
  // exits normally cannot replay buffered output.
  out.flush();
  err.flush();
  auto report = supervisor.run(config, options);
  if (!report.has_value()) {
    err << "loadforge: " << describe(report.error()) << "\n";
    return kConfigRefused;
  }
  const std::optional<int> child_code = report.value().worker_exit_code;
  if (child_code.has_value()) {
    return *child_code;  // This process is a worker that could not exec.
  }
  const bool workers_clean = controller::all_clean(report.value());

  const HostFacts host = stress_options.host.has_value() ? *stress_options.host : read_host_facts();
  const statistics::CompensatedSummarizer real_summarizer;
  const statistics::Summarizer& summarizer =
      stress_options.summarizer != nullptr ? *stress_options.summarizer : real_summarizer;
  results::FingerprintContext context;
  context.build = results::this_build();
  context.fp_environment = host.fp_environment;
  context.isa_path = host.isa_path;
  auto reading = results::read_fingerprint(machine, "", context);
  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report.value();
  inputs.fingerprint = reading.fingerprint;
  if (rig != nullptr) {
    inputs.capabilities = rig->capabilities();
  }
  inputs.timeline = &timeline;
  inputs.summarizer = &summarizer;
  auto result = assemble_result(inputs);
  if (!result.has_value()) {
    err << "loadforge: cannot compute the result: " << result.error() << "\n";
    return kOutputFailed;
  }
  out << summary_text(result.value(), report.value(), timeline, summarizer, reading.gaps);
  const int code = exit_code_for(result.value().verdict, workers_clean);
  if (stress_options.formats.empty()) {
    return code;
  }
  auto written =
      write_results(result.value(), stress_options.formats, stress_options.output_dir, sink);
  if (!written.has_value()) {
    err << "loadforge: cannot write the result: " << written.error() << "\n";
    return kOutputFailed;
  }
  for (const std::string& file : written.value()) {
    out << "result written: " << file << "\n";
  }
  return code;
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
  if (command == "--check-config") {
    if (args.size() != 2) {
      err << "loadforge: --check-config takes exactly one FILE\n";
      return kUsageError;
    }
    return check_config(args[1], out, err);
  }
  if (command == "stress") {
    const auto parsed = parse_stress_args(args.subspan(1));
    if (!parsed.has_value()) {
      err << "loadforge: " << parsed.error() << "\n";
      return kUsageError;
    }
    return stress(parsed.value().config_path, "/proc/self/exe", topology::kDefaultSysfsRoot,
                  parsed.value().options, out, err);
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
