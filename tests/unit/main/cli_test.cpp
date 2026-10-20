// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "platform/clock.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/snapshot.hpp"
#include "unit/main/cli_support.hpp"
#include "worker/entry.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"

namespace loadforge::cli {
namespace {

struct Outcome {
  int code;
  std::string out;
  std::string err;
};

Outcome invoke(const std::vector<std::string_view>& args) {
  std::ostringstream out;
  std::ostringstream err;
  const int code = run(args, out, err);
  return {code, out.str(), err.str()};
}

TEST(CollectArgs, EmptyArgvYieldsNoArguments) {
  // POSIX permits execve with an empty argv. Unguarded, subspan(1) here would
  // be undefined behaviour.
  EXPECT_TRUE(collect_args({}).empty());
}

TEST(CollectArgs, ProgramNameOnlyYieldsNoArguments) {
  std::array<char*, 1> argv{const_cast<char*>("loadforge")};
  EXPECT_TRUE(collect_args(argv).empty());
}

TEST(CollectArgs, DropsProgramNameAndKeepsTheRest) {
  std::array<char*, 3> argv{const_cast<char*>("loadforge"), const_cast<char*>("--version"),
                            const_cast<char*>("extra")};
  const auto args = collect_args(argv);
  ASSERT_EQ(args.size(), 2U);
  EXPECT_EQ(args[0], "--version");
  EXPECT_EQ(args[1], "extra");
}

TEST(Cli, NoArgumentsPrintsUsageAndSucceeds) {
  const Outcome r = invoke({});
  EXPECT_EQ(r.code, 0);
  EXPECT_NE(r.out.find("Usage:"), std::string::npos);
  EXPECT_TRUE(r.err.empty());
}

TEST(Cli, VersionFlagPrintsBareVersion) {
  for (const std::string_view flag : {"--version", "-V"}) {
    const Outcome r = invoke({flag});
    EXPECT_EQ(r.code, 0) << flag;
    EXPECT_EQ(r.out, std::string(kVersion) + "\n") << flag;
    EXPECT_TRUE(r.err.empty()) << flag;
  }
}

TEST(Cli, HelpFlagPrintsUsage) {
  for (const std::string_view flag : {"--help", "-h"}) {
    const Outcome r = invoke({flag});
    EXPECT_EQ(r.code, 0) << flag;
    EXPECT_NE(r.out.find("Usage:"), std::string::npos) << flag;
  }
}

TEST(Cli, SelftestDigestPrintsAStableHexDigest) {
  const Outcome first = invoke({"--selftest-digest"});
  EXPECT_EQ(first.code, 0);
  EXPECT_TRUE(first.err.empty());
  EXPECT_EQ(first.out.size(), 17U) << "expected 16 hex digits and a newline";

  // The whole point of this command is that the value does not move: CI
  // compares it across architectures.
  const Outcome second = invoke({"--selftest-digest"});
  EXPECT_EQ(second.out, first.out);
}

TEST(Cli, UnknownArgumentFailsAndSuggestsHelp) {
  const Outcome r = invoke({"--wat"});
  EXPECT_EQ(r.code, 2);
  EXPECT_NE(r.err.find("unknown argument"), std::string::npos);
  EXPECT_NE(r.err.find("--help"), std::string::npos);
}

TEST(Cli, TooManyArgumentsFails) {
  const Outcome r = invoke({"--version", "--help"});
  EXPECT_EQ(r.code, 2);
  EXPECT_NE(r.err.find("single argument"), std::string::npos);
}

TEST(Cli, CheckConfigTakesExactlyOneFile) {
  for (const std::vector<std::string_view>& args :
       {std::vector<std::string_view>{"--check-config"},
        std::vector<std::string_view>{"--check-config", "a.toml", "b.toml"}}) {
    const Outcome r = invoke(args);
    EXPECT_EQ(r.code, 2);
    EXPECT_NE(r.err.find("exactly one FILE"), std::string::npos);
    EXPECT_TRUE(r.out.empty());
  }
}

TEST(Cli, CheckConfigAcceptsTheShippedQuickConfig) {
  const Outcome r = invoke({"--check-config", LOADFORGE_SOURCE_DIR "/config/quick.toml"});
  EXPECT_EQ(r.code, 0) << r.err;
  EXPECT_EQ(r.out.substr(0, 4), "ok: ");
  EXPECT_NE(r.out.find("stress for 30s, 1 workload(s)"), std::string::npos) << r.out;
  EXPECT_TRUE(r.err.empty());
}

TEST(Cli, CheckConfigRefusesWithTheKeyAndLineOnStderr) {
  const Outcome r =
      invoke({"--check-config", LOADFORGE_SOURCE_DIR "/tests/fixtures/config/unknown-key.toml"});
  EXPECT_EQ(r.code, 1);
  EXPECT_TRUE(r.out.empty());
  EXPECT_NE(r.err.find("unknown-key.toml:11: limits.max_temprature_c: unknown key"),
            std::string::npos)
      << r.err;
}

TEST(Cli, CheckConfigReportsAMissingFile) {
  const Outcome r = invoke({"--check-config", LOADFORGE_SOURCE_DIR "/config/absent.toml"});
  EXPECT_EQ(r.code, 1);
  EXPECT_NE(r.err.find("absent.toml: cannot open the file"), std::string::npos) << r.err;
}

TEST(Cli, TheWorkerSwitchRefusesBadArgumentsWithItsOwnCode) {
  const Outcome r = invoke({"--worker"});
  EXPECT_EQ(r.code, 125);
  EXPECT_EQ(r.err, "loadforge --worker: --slot: required\n");
  EXPECT_TRUE(r.out.empty());
}

TEST(Cli, TheWorkerSwitchRunsARealWorkerInThisProcess) {
  // The controller's side, for real: a memfd region, two pipes, a beat in
  // slot 0, and a stop request already on the control pipe, so the worker
  // attaches, reports, takes the request and exits complete -- through the
  // same dispatch main() uses, over the real seam.
  platform::RealSyscalls syscalls;
  auto created = ipc::HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(created.has_value()) << describe(created.error());
  ipc::HeartbeatRegion region = std::move(created).value();
  platform::Clock clock(syscalls);
  auto now = clock.now();
  ASSERT_TRUE(now.has_value());
  ASSERT_TRUE(
      region
          .publish(ipc::kControllerSlot, ipc::Heartbeat{now.value().running_ns, 1, 1,
                                                        static_cast<std::uint32_t>(::getpid())})
          .has_value());
  auto records = syscalls.create_pipe();
  auto control = syscalls.create_pipe();
  ASSERT_TRUE(records.has_value() && control.has_value());
  const std::array<std::byte, 1> stop{std::byte{'s'}};
  ASSERT_TRUE(syscalls.write(control.value().write_fd, stop).has_value());

  worker::WorkerArgs args;
  args.slot = 0;
  args.workers = 1;
  args.heartbeat_fd = region.fd();
  args.record_fd = records.value().write_fd;
  args.control_fd = control.value().read_fd;
  args.supervisor_pid = ::getpid();
  args.pacing = worker::Pacing{5'000'000'000, 10'000'000, 1'000'000'000};
  std::vector<std::string> argv = worker::argv_for("loadforge", args);
  argv.erase(argv.begin());  // The executable; run() takes what follows it.
  const std::vector<std::string_view> views(argv.begin(), argv.end());
  const Outcome r = invoke(views);
  EXPECT_EQ(r.code, 0) << r.err;
  EXPECT_TRUE(r.err.empty()) << r.err;

  ASSERT_TRUE(syscalls.close(records.value().write_fd).has_value());
  ipc::RecordSource source(syscalls, records.value().read_fd);
  std::vector<ipc::Record> received;
  while (!source.at_end()) {
    auto drained = source.drain();
    ASSERT_TRUE(drained.has_value()) << describe(drained.error());
    received.insert(received.end(), drained.value().records.begin(), drained.value().records.end());
  }
  ASSERT_EQ(received.size(), 2U);
  EXPECT_EQ(received[0].type, static_cast<std::uint16_t>(ipc::RecordType::kStarted));
  EXPECT_EQ(received[1].type, static_cast<std::uint16_t>(ipc::RecordType::kFinished));
  std::string summary;
  for (const std::byte b : received[1].payload) {
    summary.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
  }
  EXPECT_EQ(summary.substr(0, 31), "the controller asked for a stop");
  auto beat = region.observe(ipc::worker_slot(0));
  ASSERT_TRUE(beat.has_value() && beat.value().has_value());
  EXPECT_EQ(beat.value()->pid, static_cast<std::uint32_t>(::getpid()));
  EXPECT_TRUE(syscalls.close(records.value().read_fd).has_value());
  EXPECT_TRUE(syscalls.close(control.value().read_fd).has_value());
  EXPECT_TRUE(syscalls.close(control.value().write_fd).has_value());
}

TEST(Cli, StressTakesAConfigAndNothingItDoesNotKnow) {
  for (const auto& [args, message] :
       {std::pair{std::vector<std::string_view>{"stress"}, "stress needs --config FILE"},
        std::pair{std::vector<std::string_view>{"stress", "--conf", "a.toml"},
                  "stress does not take --conf"},
        std::pair{std::vector<std::string_view>{"stress", "--config", "a.toml", "extra"},
                  "stress does not take extra"},
        std::pair{std::vector<std::string_view>{"stress", "--config"}, "--config takes a value"},
        std::pair{std::vector<std::string_view>{"stress", "--config", "a", "--config", "b"},
                  "--config is given twice"},
        std::pair{std::vector<std::string_view>{"stress", "--config", "a", "--output", "xml"},
                  "unknown output format \"xml\""},
        std::pair{
            std::vector<std::string_view>{"stress", "--config", "a", "--sample-interval", "soon"},
            "--sample-interval: "},
        std::pair{
            std::vector<std::string_view>{"stress", "--config", "a", "--sample-interval", "0s"},
            "--sample-interval must be above zero"},
        std::pair{std::vector<std::string_view>{"stress", "--config", "a", "--sample-interval",
                                                "9223372036854775807ms"},
                  "--sample-interval is longer than any run can be"}}) {
    const Outcome r = invoke(args);
    EXPECT_EQ(r.code, 2);
    EXPECT_NE(r.err.find(message), std::string::npos) << r.err;
    EXPECT_TRUE(r.out.empty());
  }
}

TEST(Cli, StressRefusesAMissingFileAndAModeThisBuildDoesNotRun) {
  const Outcome missing =
      invoke({"stress", "--config", LOADFORGE_SOURCE_DIR "/config/absent.toml"});
  EXPECT_EQ(missing.code, 1);
  EXPECT_NE(missing.err.find("absent.toml: cannot open the file"), std::string::npos)
      << missing.err;

  const Outcome benchmark = invoke(
      {"stress", "--config", LOADFORGE_SOURCE_DIR "/tests/fixtures/config/benchmark-mode.toml"});
  EXPECT_EQ(benchmark.code, 1);
  EXPECT_NE(benchmark.err.find("mode benchmark lands with a later milestone"), std::string::npos)
      << benchmark.err;
  EXPECT_TRUE(benchmark.out.empty());
}

TEST(Cli, StressRefusesToRunWhenItCannotNameTheWorkerExecutable) {
  // A live medium without /proc is where a wrong worker binary would be
  // hardest to notice, so there is no fallback: the run is refused and says why.
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(stress(LOADFORGE_SOURCE_DIR "/tests/fixtures/config/short-stress.toml",
                   LOADFORGE_SOURCE_DIR "/no-such-link", topology::kDefaultSysfsRoot,
                   StressOptions{}, out, err),
            1);
  EXPECT_NE(err.str().find("cannot resolve this executable for the workers: readlink("),
            std::string::npos)
      << err.str();
  EXPECT_TRUE(out.str().empty());
}

TEST(Cli, StressRefusesAMachineItCannotDescribeBeforeAnyWorkerStarts) {
  // The topology snapshot is the frame the run is reported against, and it
  // is taken before the first fork (docs/PLAN.md §4.1): a /sys that will not
  // describe the machine refuses the run while no worker exists. A real
  // executable link, so the snapshot is the only thing that can refuse.
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(
      stress(LOADFORGE_SOURCE_DIR "/tests/fixtures/config/short-stress.toml", "/proc/self/exe",
             LOADFORGE_SOURCE_DIR "/no-such-sysfs", StressOptions{}, out, err),
      1);
  EXPECT_NE(err.str().find("loadforge: cannot describe this machine before starting workers: " +
                           std::string{LOADFORGE_SOURCE_DIR} +
                           "/no-such-sysfs/devices/system/cpu/online"),
            std::string::npos)
      << err.str();
  EXPECT_TRUE(out.str().empty()) << "no banner, so no run: " << out.str();
}

/// The supervisor alone: these tests script the seam's reads in the order the
/// budget and the OOM record make them, and a source probed first would take
/// them. The telemetry is tested where its files are served by path.
const StressOptions& supervisor_only() {
  static const StressOptions options = [] {
    StressOptions only;
    only.sample = false;
    return only;
  }();
  return options;
}

TEST(Cli, RunStressReturnsAFailedWorkersExitCodeInTheChild) {
  testing::FakeSyscalls syscalls;
  syscalls.advance_clocks_per_read(10'000'000);
  syscalls.script_forks({{0, 0}});  // This process is the worker; its exec will fail.
  syscalls.set_parent_pid(testing::FakeSyscalls::kFakePid);
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, two_workloads(), "/opt/loadforge", one_cpu_machine(),
                       supervisor_only(), out, err),
            120);
  EXPECT_TRUE(err.str().empty()) << err.str();
  EXPECT_EQ(syscalls.exec_count(), 1);
}

TEST(Cli, RunStressReportsAControllerFailureAndTheUncleanRun) {
  testing::FakeSyscalls failing;
  failing.advance_clocks_per_read(10'000'000);
  failing.script_forks({{0, EAGAIN}});
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(failing, two_workloads(), "/opt/loadforge", one_cpu_machine(),
                       supervisor_only(), out, err),
            1);
  EXPECT_NE(err.str().find("loadforge: spawn: fork(worker process): Resource temporarily"),
            std::string::npos)
      << err.str();

  // Two workers that exit at once with nothing sent: the run ends, and it is
  // not clean, which is its own exit code.
  testing::FakeSyscalls unclean;
  unclean.advance_clocks_per_read(10'000'000);
  unclean.script_forks({{1001, 0}, {1002, 0}});
  unclean.script_nonblocking_reaps(
      {{platform::Reaped{1001, 0}, 0}, {platform::Reaped{1002, 0}, 0}});
  std::ostringstream report;
  std::ostringstream quiet;
  EXPECT_EQ(run_stress(unclean, two_workloads(), "/opt/loadforge", one_cpu_machine(),
                       supervisor_only(), report, quiet),
            3);
  EXPECT_NE(report.str().find("0 of 2 worker(s) completed"), std::string::npos) << report.str();
  EXPECT_NE(report.str().find("\n  topology: 1 logical CPU(s) on 1 core(s) in 1 package(s); "
                              "caches not exposed; NUMA not exposed\n"),
            std::string::npos)
      << "the snapshot it was given reaches the banner: " << report.str();
  EXPECT_NE(report.str().find("evidence truncated"), std::string::npos) << report.str();
  EXPECT_TRUE(quiet.str().empty()) << quiet.str();
}

// --- the memory budget and the OOM-kill record, read through the run's seam ----------
//
// run_stress reads both through the seam it is given, so over the fake they
// come from the fake's read script, in the order the readers open their
// files: /proc/meminfo, then /proc/self/cgroup for the budget, then
// /proc/self/cgroup again for the record. An empty /proc/self/cgroup names no
// memory cgroup. How each source is read is the reader's own business and its
// own tests'; these check that what it reads reaches the controller.

/// Two workers that exit at once with nothing sent.
testing::FakeSyscalls& two_quick_exits(testing::FakeSyscalls& syscalls, int first_status) {
  syscalls.advance_clocks_per_read(10'000'000);
  syscalls.script_forks({{1001, 0}, {1002, 0}});
  syscalls.script_nonblocking_reaps(
      {{platform::Reaped{1001, first_status}, 0}, {platform::Reaped{1002, 0}, 0}});
  return syscalls;
}

void serve_machine(testing::FakeSyscalls& syscalls, const std::string& meminfo) {
  syscalls.script_reads({{meminfo, 0}, {"", 0}, {"", 0}, {"", 0}});
}

TEST(Cli, RunStressChecksMemoryAgainstTheBudgetItReads) {
  config::Config config = two_workloads();
  config.workloads[0].memory = core::ByteSize::bytes(std::uint64_t{512} << 20U);
  testing::FakeSyscalls syscalls;
  serve_machine(two_quick_exits(syscalls, 0), "MemAvailable:   1048576 kB\n");
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, config, "/opt/loadforge", one_cpu_machine(), supervisor_only(),
                       out, err),
            3);
  EXPECT_NE(out.str().find("limit memory: enforced (checked at start against a ceiling of 1GiB)"),
            std::string::npos)
      << out.str();

  config.workloads[0].memory = core::ByteSize::bytes(std::uint64_t{2} << 30U);
  testing::FakeSyscalls refusing;
  serve_machine(two_quick_exits(refusing, 0), "MemAvailable:   1048576 kB\n");
  std::ostringstream quiet;
  std::ostringstream refusal;
  EXPECT_EQ(run_stress(refusing, config, "/opt/loadforge", one_cpu_machine(), supervisor_only(),
                       quiet, refusal),
            1);
  EXPECT_NE(refusal.str().find("loadforge: memory: workload.first: "), std::string::npos)
      << refusal.str();
  EXPECT_EQ(refusing.fork_count(), 0) << "refused before any worker started";
}

TEST(Cli, RunStressSaysWhyMemoryIsNotEnforcedWhenTheBudgetCannotBeRead) {
  config::Config config = two_workloads();
  config.limits.memory = core::ByteSize::bytes(std::uint64_t{512} << 20U);
  testing::FakeSyscalls syscalls;
  serve_machine(two_quick_exits(syscalls, 0), "MemTotal:       16479424 kB\n");
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, config, "/opt/loadforge", one_cpu_machine(), supervisor_only(),
                       out, err),
            3);
  EXPECT_NE(out.str().find("limit memory: NOT ENFORCED (no memory budget could be read: "
                           "/proc/meminfo: not offered by this kernel (no MemAvailable line"),
            std::string::npos)
      << out.str();
}

TEST(Cli, RunStressConsultsTheOomRecordForASigkillNobodySent) {
  testing::FakeSyscalls syscalls;
  serve_machine(two_quick_exits(syscalls, SIGKILL), "MemAvailable:   1048576 kB\n");
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, two_workloads(), "/opt/loadforge", one_cpu_machine(),
                       supervisor_only(), out, err),
            3);
  EXPECT_NE(out.str().find("worker 0 (pid 1001): killed; killed by signal 9"), std::string::npos)
      << out.str();
  EXPECT_NE(out.str().find("not attributed to the OOM killer: the OOM-kill count could not be "
                           "read before the run: /proc/self/cgroup: not offered by this kernel "
                           "(this process is in no memory cgroup"),
            std::string::npos)
      << out.str();
}

TEST(Cli, DiagnosticsGoToStderrNotStdout) {
  // A caller piping --version into a script must never receive an error message
  // on the same stream.
  const Outcome r = invoke({"--nope"});
  EXPECT_TRUE(r.out.empty());
  EXPECT_FALSE(r.err.empty());
}

}  // namespace
}  // namespace loadforge::cli
