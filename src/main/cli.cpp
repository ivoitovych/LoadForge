// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "controller/orchestration.hpp"
#include "core/duration.hpp"
#include "main/selftest.hpp"
#include "platform/fs.hpp"
#include "platform/process.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "reporters/console.hpp"
#include "resources/cgroup.hpp"
#include "resources/cgroup_oom_record.hpp"
#include "resources/memory_budget.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/snapshot.hpp"
#include "worker/entry.hpp"

namespace loadforge::cli {
namespace {

constexpr int kOk = 0;
constexpr int kConfigRefused = 1;  ///< Also: a run that could not start.
constexpr int kUsageError = 2;
constexpr int kRunNotClean = 3;  ///< The run ended and at least one worker did not complete.

void print_usage(std::ostream& out) {
  out << "loadforge " << kVersion << "\n"
      << "\n"
      << "Usage:\n"
      << "  loadforge --version\n"
      << "  loadforge --help\n"
      << "  loadforge --check-config FILE   parse and validate a configuration\n"
      << "  loadforge stress --config FILE  run the null workload under supervision\n"
      << "\n"
      << "Exit codes: 0 clean; 1 refused (configuration, or the run could not start);\n"
      << "2 usage; 3 the run ended with a worker that did not complete.\n"
      << "See docs/PLAN.md for what lands when.\n";
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
  return run_stress(syscalls, config, executable.value(), machine.value(), out, err);
}

int run_stress(platform::Syscalls& syscalls, const config::Config& config,
               const std::string& executable, const topology::Snapshot& snapshot, std::ostream& out,
               std::ostream& err) {
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
  return controller::all_clean(report.value()) ? kOk : kRunNotClean;
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
    if (args.size() != 3 || args[1] != "--config") {
      err << "loadforge: stress takes exactly --config FILE\n";
      return kUsageError;
    }
    return stress(args[2], "/proc/self/exe", topology::kDefaultSysfsRoot, out, err);
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
