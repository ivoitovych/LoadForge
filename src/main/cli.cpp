// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "controller/isolate.hpp"
#include "controller/orchestration.hpp"
#include "controller/rerun.hpp"
#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "core/parse_error.hpp"
#include "core/parse_number.hpp"
#include "main/outcome.hpp"
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
#include "telemetry/sources/fan/fan.hpp"
#include "telemetry/sources/frequency/frequency.hpp"
#include "telemetry/sources/memory/memory.hpp"
#include "telemetry/sources/thermal/thermal.hpp"
#include "topology/cache.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/snapshot.hpp"
#include "verification/isolation/ladder.hpp"
#include "worker/entry.hpp"
#include "worker/services.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::cli {
namespace {

constexpr int kOk = 0;
constexpr int kConfigRefused = 1;  ///< Also: a run that could not start.
constexpr int kUsageError = 2;
constexpr int kRunNotClean = 3;  ///< The run ended and at least one worker did not complete.

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
      << "  loadforge stress --config FILE  run a configuration under supervision\n"
      << "  loadforge stress --test NAME [--duration D] [--workers N] [--footprint SIZE]\n"
      << "                   [--isa scalar|avx2|avx512|neon] [--seed N]\n"
      << "                                  stress with one verified workload; NAME is one of\n"
      << "                                  compression, dense_linear, monte_carlo, sort\n"
      << "\n"
      << "Exit codes: 0 clean; 1 refused (configuration, or the run could not start);\n"
      << "2 usage; 3 the run ended with a worker that did not complete; 4 a wrong result\n"
      << "was found; 5 a check was not performed (a run that checked nothing is not clean).\n"
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
};

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
    const std::string_view value = args[index + 1];
    if (key == "--duration" && !parsed.duration.has_value()) {
      auto duration = core::parse_duration(value);
      if (!duration.has_value()) {
        return refused(key, std::string{core::describe(duration.error())});
      }
      parsed.duration = duration.value();
    } else if (key == "--workers" && !parsed.workers.has_value()) {
      auto count = whole_number(key, value);
      if (!count.has_value()) {
        return count.error();
      }
      if (count.value() == 0) {
        return refused(key, "must be at least 1");
      }
      parsed.workers = count.value();
    } else if (key == "--footprint" && !parsed.footprint.has_value()) {
      auto size = core::parse_byte_size(value);
      if (!size.has_value()) {
        return refused(key, std::string{core::describe(size.error())});
      }
      parsed.footprint = size.value();
    } else if (key == "--isa" && !parsed.isa.has_value()) {
      const std::optional<simd::IsaPath> path = simd::parse_isa_path(value);
      if (!path.has_value()) {
        return refused(key, "'" + std::string{value} + "' is none of scalar, avx2, avx512, neon");
      }
      parsed.isa = path;
    } else if (key == "--seed" && !parsed.seed.has_value()) {
      auto seed = whole_number(key, value);
      if (!seed.has_value()) {
        return seed.error();
      }
      parsed.seed = seed.value();
    } else {
      return refused(key, "not an option of stress --test, or given twice");
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
  return Launch{executable.value(), machine.value()};
}

}  // namespace

int stress(std::string_view path, const std::string& self_link, std::string_view sysfs_root,
           std::ostream& out, std::ostream& err) {
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
  return run_stress(syscalls, config, launch->executable, launch->machine, StressOptions{}, out,
                    err);
}

int stress_test(std::span<const std::string_view> args, const std::string& self_link,
                std::string_view sysfs_root, std::ostream& out, std::ostream& err) {
  auto parsed = parse_test_args(args);
  if (!parsed.has_value()) {
    err << "loadforge: stress --test: " << parsed.error() << "\n";
    return kUsageError;
  }
  const TestArgs& test = parsed.value();
  const workloads::BuiltinRegistry registry;
  const std::vector<std::string> names = registry.names();
  if (std::ranges::find(names, test.workload) == names.end()) {
    err << "loadforge: stress --test: no workload named '" << test.workload << "'; this build has:";
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
  config::Config config;
  config.run.mode = config::Mode::kStress;
  config.run.duration = test.duration.value_or(core::Duration{kDefaultTestMilliseconds});
  // One worker per CPU the process may use, each pinned to its own: the
  // placement a finding needs to be repeated on another core.
  const std::size_t workers = test.workers.has_value()
                                  ? static_cast<std::size_t>(*test.workers)
                                  : std::clamp<std::size_t>(cpus.size(), 1, kMostDefaultWorkers);
  for (std::size_t index = 0; index < workers; ++index) {
    config.workloads.push_back(
        config::Workload{test.workload, config::Threads::count(1), true, test.footprint});
  }
  StressOptions options;
  options.isa = test.isa;
  options.seed = test.seed.value_or(options.seed);
  return run_stress(syscalls, config, launch->executable, launch->machine, options, out, err);
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

  // The verified runs: the workloads this build has, the ISA path chosen once
  // from what this CPU can run, the floating-point registers established and
  // asserted before anything starts, and the telemetry the machine offers
  // probed once, for the banner (M2a exit criteria 1 and 5; M2b 4).
  const workloads::BuiltinRegistry registry;
  const std::unique_ptr<simd::IsaProbe> probe = isa::make_native_probe();
  fpenv::RealFpControl fp;
  telemetry::ThermalSource thermal(machine);
  telemetry::FanSource fan(machine);
  telemetry::FrequencySource frequency(machine);
  telemetry::MemorySource memory(machine);
  const std::vector<std::string> registered = registry.names();
  const bool verifying = std::ranges::any_of(config.workloads, [&registered](const config::Workload& w) {
    return w.enabled && std::ranges::find(registered, w.name) != registered.end();
  });
  if (verifying) {
    options.stale_after_ns = std::max(options.stale_after_ns, kVerifyingStaleAfterNs);
  }
  options.verification.registry = &registry;
  options.verification.seed = stress_options.seed;
  options.verification.footprint_bytes = stress_options.footprint_bytes;
  options.verification.requested_isa = stress_options.isa;
  options.verification.isa_probe = probe.get();
  options.verification.line_bytes = line_bytes_of(snapshot);
  options.verification.cpus = allowed_cpus(syscalls, snapshot);
  options.fp = &fp;
  if (stress_options.capabilities) {
    options.sources = {&thermal, &fan, &frequency, &memory};
  }

  reporters::ConsoleReporter reporter(out);
  controller::Controller supervisor(syscalls, reporter);
  // Flushed before the controller forks, so a child that fails to exec and
  // exits normally cannot replay buffered output.
  out.flush();
  err.flush();
  auto ran = supervisor.run(config, options);
  if (!ran.has_value()) {
    err << "loadforge: " << describe(ran.error()) << "\n";
    return kConfigRefused;
  }
  controller::RunReport report = std::move(ran).value();
  const std::optional<int> child_code = report.worker_exit_code;
  if (child_code.has_value()) {
    return *child_code;  // This process is a worker that could not exec.
  }
  if (controller::wrong_result_count(report) != 0) {
    // The workers are gone: what is left to do is repeat the units that came
    // out wrong, in processes of their own, and say what that shows.
    verification::isolation::LadderIsolator isolator;
    controller::RealRerunner rerunner(syscalls, executable, controller::RerunOptions{});
    const controller::IsolationSummary isolated = controller::isolate_findings(
        report, &snapshot, isolator, rerunner, stress_options.isolation_limit);
    if (isolated.child_exit_code.has_value()) {
      return *isolated.child_exit_code;  // A re-run's exec failed in this process.
    }
    reporter.isolation_finished(report);
  }
  return exit_code_for(report, config);
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
    if (args.size() == 3 && args[1] == "--config") {
      return stress(args[2], "/proc/self/exe", topology::kDefaultSysfsRoot, out, err);
    }
    if (args.size() >= 3 && args[1] == "--test") {
      return stress_test(args.subspan(2), "/proc/self/exe", topology::kDefaultSysfsRoot, out, err);
    }
    err << "loadforge: stress takes --config FILE or --test NAME [options]\n";
    return kUsageError;
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
