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
#include <iterator>
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
#include "unit/controller/machine_tree.hpp"
#include "unit/main/cli_support.hpp"
#include "worker/control.hpp"
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
  const std::vector<std::byte> stop_payload =
      worker::encode(worker::ControlMessage{1, worker::Stop{}});
  const std::vector<std::byte> stop = ipc::encode(worker::kControlFrameType, stop_payload).value();
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
  ASSERT_EQ(received.size(), 3U);
  EXPECT_EQ(received[0].type, static_cast<std::uint16_t>(ipc::RecordType::kStarted));
  EXPECT_EQ(received[1].type, worker::kControlAckFrameType) << "the stop is answered";
  const auto answered = worker::decode_ack(received[1].payload);
  ASSERT_TRUE(answered.has_value());
  EXPECT_EQ(answered.value(), (worker::ControlAck{1, worker::AckStatus::kDone, ""}));
  EXPECT_EQ(received[2].type, static_cast<std::uint16_t>(ipc::RecordType::kFinished));
  std::string summary;
  for (const std::byte b : received[2].payload) {
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
    only.capabilities = false;
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

// --- stress --test ----------------------------------------------------------------------

TEST(CliStressTest, AnUnknownWorkloadIsAUsageErrorThatListsThoseThisBuildHas) {
  const Outcome r = invoke({"stress", "--test", "ffts"});
  EXPECT_EQ(r.code, 2);
  EXPECT_EQ(r.err,
            "loadforge: stress --test: no workload named 'ffts'; this build has: adaptive_mesh "
            "compression dense_linear fft graph monte_carlo particles raytrace sort sparse "
            "stencil\n");
  EXPECT_TRUE(r.out.empty());
}

TEST(CliStressTest, EveryOptionIsRefusedByNameWhenItIsMisused) {
  struct Case {
    std::vector<std::string_view> options;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {{"--duration"}, "--duration: missing its value"},
      {{"--duration", "soon"}, "--duration: "},
      {{"--workers", "0"}, "--workers: must be at least 1"},
      {{"--workers", "many"}, "--workers: expected a whole number, got 'many'"},
      {{"--workers", "2x"}, "--workers: expected a whole number, got '2x'"},
      {{"--footprint", "big"}, "--footprint: "},
      {{"--isa", "sse"}, "--isa: 'sse' is none of scalar, avx2, avx512, neon"},
      {{"--seed", "-1"}, "--seed: expected a whole number, got '-1'"},
      {{"--bogus", "1"}, "--bogus: not an option of stress --test, or given twice"},
      {{"--workers", "1", "--workers", "2"},
       "--workers: not an option of stress --test, or given twice"},
      {{"--seed", "1", "--seed", "2"}, "--seed: not an option of stress --test, or given twice"},
      {{"--duration", "1s", "--duration", "2s"},
       "--duration: not an option of stress --test, or given twice"},
      {{"--footprint", "1MiB", "--footprint", "2MiB"},
       "--footprint: not an option of stress --test, or given twice"},
      {{"--isa", "scalar", "--isa", "neon"},
       "--isa: not an option of stress --test, or given twice"},
      {{"--lock", "always"}, "--lock: 'always' is none of require, try, none"},
      {{"--lock", "try", "--lock", "none"},
       "--lock: not an option of stress --test, or given twice"},
  };
  for (const Case& each : cases) {
    std::vector<std::string_view> args{"stress", "--test", "compression"};
    args.insert(args.end(), each.options.begin(), each.options.end());
    const Outcome r = invoke(args);
    EXPECT_EQ(r.code, 2) << each.expected;
    EXPECT_EQ(r.err.substr(0, 26 + each.expected.size()),
              "loadforge: stress --test: " + each.expected)
        << r.err;
    EXPECT_TRUE(r.out.empty());
  }
}

TEST(CliStressTest, StressTakesAConfigOrATestAndNothingElse) {
  for (const auto& [args, message] :
       {std::pair{std::vector<std::string_view>{"stress", "--test"}, "stress does not take --test"},
        std::pair{std::vector<std::string_view>{"stress", "--tests", "compression"},
                  "stress does not take --tests"}}) {
    const Outcome r = invoke(args);
    EXPECT_EQ(r.code, 2);
    EXPECT_NE(r.err.find(message), std::string::npos) << r.err;
  }
}

/// A link to the shipped binary, which is what a real run's workers must be:
/// in the unit tests /proc/self/exe is the test.
class LinkToTheBinary {
 public:
  LinkToTheBinary()
      : path_(std::filesystem::temp_directory_path() /
              ("loadforge-link-" + std::to_string(::getpid()))) {
    std::filesystem::remove(path_);
    std::filesystem::create_symlink(LOADFORGE_BINARY, path_);
  }
  ~LinkToTheBinary() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  LinkToTheBinary(const LinkToTheBinary&) = delete;
  LinkToTheBinary& operator=(const LinkToTheBinary&) = delete;
  LinkToTheBinary(LinkToTheBinary&&) = delete;
  LinkToTheBinary& operator=(LinkToTheBinary&&) = delete;
  [[nodiscard]] std::string path() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

TEST(CliStressTest, ATestThatCannotNameItsExecutableOrDescribeTheMachineIsRefusedBeforeAnyWorker) {
  {
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string_view> args{"compression", "--duration", "1s"};
    EXPECT_EQ(
        stress_test(args, LOADFORGE_SOURCE_DIR "/no-such-link", "/sys", StressOptions{}, out, err),
        1);
    EXPECT_NE(err.str().find("cannot resolve this executable for the workers"), std::string::npos)
        << err.str();
  }
  {
    const LinkToTheBinary link;
    std::ostringstream out;
    std::ostringstream err;
    const std::vector<std::string_view> args{"compression", "--duration", "1s"};
    EXPECT_EQ(stress_test(args, link.path(), LOADFORGE_SOURCE_DIR "/no-such-sysfs", StressOptions{},
                          out, err),
              1);
    EXPECT_NE(err.str().find("cannot describe this machine before starting workers"),
              std::string::npos)
        << err.str();
    EXPECT_TRUE(out.str().empty());
  }
}

TEST(CliStressTest, ATestRunsRealWorkersForRealAndReportsWhatTheyChecked) {
  const LinkToTheBinary link;
  std::ostringstream out;
  std::ostringstream err;
  const std::vector<std::string_view> args{"compression", "--duration",  "1s",    "--workers",
                                           "2",           "--footprint", "6MiB",  "--seed",
                                           "7",           "--isa",       "scalar"};
  EXPECT_EQ(stress_test(args, link.path(), "/sys", StressOptions{}, out, err), 0)
      << out.str() << err.str();
  const std::string text = out.str();
  EXPECT_NE(text.find("stress for 1s with 2 worker(s)\n"), std::string::npos) << text;
  EXPECT_NE(text.find("  workload compression: 1 thread(s) on cpu "), std::string::npos) << text;
  EXPECT_NE(text.find("footprint 6MiB"), std::string::npos) << text;
  EXPECT_NE(text.find("  floating point (asserted at startup): rounding to nearest"),
            std::string::npos)
      << text;
  const std::size_t capabilities = text.find("telemetry capability: ");
  ASSERT_NE(capabilities, std::string::npos) << text;
  EXPECT_LT(capabilities, text.find("stress for 1s")) << "stated once, before the run";
  EXPECT_EQ(text.find("telemetry capability: ", capabilities + 1), std::string::npos)
      << "and the sources are not probed a second time for the banner";
  EXPECT_NE(text.find("2 of 2 worker(s) completed"), std::string::npos) << text;
  EXPECT_NE(text.find("the oracle checked "), std::string::npos) << text;
  EXPECT_NE(text.find(" sampled block(s) and found 0 wrong"), std::string::npos) << text;
  EXPECT_NE(text.find("    findings: 0 wrong result(s), 0 check(s) not performed\n"),
            std::string::npos)
      << text;
}

TEST(CliStressTest, WithoutWorkersOrAFootprintATestTakesOneWorkerPerCpuTheProcessMayUse) {
  const LinkToTheBinary link;
  std::ostringstream out;
  std::ostringstream err;
  const std::vector<std::string_view> args{"compression", "--duration", "1s", "--footprint",
                                           "4MiB"};
  EXPECT_EQ(stress_test(args, link.path(), "/sys", StressOptions{}, out, err), 0)
      << out.str() << err.str();
  EXPECT_NE(out.str().find("worker(s) completed"), std::string::npos) << out.str();
}

TEST(CliStressTest, EveryOutputOptionIsRefusedByNameWhenItIsMisused) {
  struct Case {
    std::vector<std::string_view> options;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {{"--output", "xml"}, "unknown output format \"xml\""},
      {{"--output", "json", "--output", "csv"},
       "--output: not an option of stress --test, or given twice"},
      {{"--output-dir", "a", "--output-dir", "b"},
       "--output-dir: not an option of stress --test, or given twice"},
      {{"--sample-interval", "soon"}, "--sample-interval: "},
      {{"--sample-interval", "0s"}, "--sample-interval must be above zero"},
      {{"--sample-interval", "9223372036854775807ms"},
       "--sample-interval is longer than any run can be"},
      {{"--sample-interval", "1s", "--sample-interval", "2s"},
       "--sample-interval: not an option of stress --test, or given twice"},
  };
  for (const Case& each : cases) {
    std::vector<std::string_view> args{"stress", "--test", "compression"};
    args.insert(args.end(), each.options.begin(), each.options.end());
    const Outcome r = invoke(args);
    EXPECT_EQ(r.code, 2) << each.expected;
    EXPECT_EQ(r.err.substr(0, 26 + each.expected.size()),
              "loadforge: stress --test: " + each.expected)
        << r.err;
    EXPECT_TRUE(r.out.empty());
  }
}

TEST(CliStressTest, ARunThatSamplesNothingStatesWhatTheMachineOffersInItsBannerAlone) {
  const LinkToTheBinary link;
  std::ostringstream out;
  std::ostringstream err;
  StressOptions base;
  base.sample = false;
  const std::vector<std::string_view> args{"compression", "--duration",  "1s",  "--workers",
                                           "2",           "--footprint", "6MiB"};
  EXPECT_EQ(stress_test(args, link.path(), "/sys", base, out, err), 0) << out.str() << err.str();
  const std::string text = out.str();
  const std::size_t banner = text.find("  capabilities:\ntelemetry capability: ");
  ASSERT_NE(banner, std::string::npos) << text;
  EXPECT_EQ(text.find("telemetry capability: ", banner + 20), std::string::npos)
      << "stated once, by the controller, which is the only prober";
  EXPECT_EQ(text.find("result written"), std::string::npos);
}

TEST(CliStressTest, AVerifiedRunSamplesTelemetryAndWritesTheResultWhenAskedTo) {
  const LinkToTheBinary link;
  const std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                          ("loadforge-cli-result-" + std::to_string(::getpid()));
  std::filesystem::create_directories(directory);
  std::ostringstream out;
  std::ostringstream err;
  const std::string where = directory.string();
  const std::vector<std::string_view> args{
      "compression", "--duration", "1s",       "--workers",    "2",   "--footprint",
      "6MiB",        "--output",   "json,csv", "--output-dir", where, "--sample-interval",
      "100ms"};
  EXPECT_EQ(stress_test(args, link.path(), "/sys", StressOptions{}, out, err), 0)
      << out.str() << err.str();
  const std::string text = out.str();
  EXPECT_NE(text.find("the oracle checked "), std::string::npos) << "the verified run: " << text;
  EXPECT_NE(text.find("result written: " + where + "/run.json\n"), std::string::npos) << text;
  EXPECT_NE(text.find("result written: " + where + "/run.csv\n"), std::string::npos) << text;
  std::ifstream json(directory / "run.json");
  const std::string document((std::istreambuf_iterator<char>(json)),
                             std::istreambuf_iterator<char>());
  EXPECT_NE(document.find("\"compression\""), std::string::npos)
      << "the workload that was verified is in the result";
  EXPECT_NE(document.find("\"signature\""), std::string::npos)
      << "and so is the telemetry sampled while it ran";
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

// --- the plan, over the fake -----------------------------------------------------------------

TEST(CliStressTest, EveryChoiceReachesTheWorkerThroughTheArgv) {
  const controller_testing::MachineTree tree(controller_testing::MachineShape{3, 128, true});
  const topology::Snapshot machine = tree.accepted();
  testing::FakeSyscalls syscalls;
  syscalls.advance_clocks_per_read(10'000'000);
  syscalls.script_forks({{0, 0}});  // This process is the first worker; its exec fails.
  syscalls.set_parent_pid(testing::FakeSyscalls::kFakePid);
  config::Config config = two_workloads();
  config.workloads[0].name = "compression";
  config.workloads[1].name = "compression";
  config.workloads[0].threads = config::Threads::count(1);
  config.workloads[1].threads = config::Threads::count(1);
  StressOptions options = supervisor_only();
  options.seed = 4;
  options.isa = simd::IsaPath::kScalar;
  options.footprint_bytes = std::uint64_t{4} << 20U;
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, config, "/opt/loadforge", machine, options, out, err), 120);
  const std::vector<std::string>& argv = syscalls.last_exec_argv();
  std::vector<std::string_view> tail;
  for (std::size_t at = 2; at < argv.size(); ++at) {
    tail.emplace_back(argv[at]);
  }
  auto parsed = worker::parse_args(tail);
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  ASSERT_TRUE(parsed.value().workload.has_value());
  EXPECT_EQ(parsed.value().workload->seed, 4U);
  EXPECT_EQ(parsed.value().workload->line_bytes, 128U) << "the topology's, never an assumed 64";
  EXPECT_EQ(parsed.value().workload->footprint_bytes, std::uint64_t{4} << 20U);
  EXPECT_EQ(parsed.value().workload->numa_node, 0U);
  EXPECT_EQ(parsed.value().cpus.size(), 1U);
  EXPECT_EQ(parsed.value().pacing.stale_after_ns, 10'000 * 1'000'000LL)
      << "a unit has no upper bound, so a verifying worker is not called a hang in two seconds";
}

TEST(CliStressTest, AWorkerOfTheNullWorkloadIsStillCalledAHangAfterTwoSeconds) {
  testing::FakeSyscalls syscalls;
  syscalls.advance_clocks_per_read(10'000'000);
  syscalls.script_forks({{0, 0}});
  syscalls.set_parent_pid(testing::FakeSyscalls::kFakePid);
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, two_workloads(), "/opt/loadforge", one_cpu_machine(),
                       supervisor_only(), out, err),
            120);
  std::vector<std::string_view> tail;
  const std::vector<std::string>& argv = syscalls.last_exec_argv();
  for (std::size_t at = 2; at < argv.size(); ++at) {
    tail.emplace_back(argv[at]);
  }
  auto parsed = worker::parse_args(tail);
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value().pacing.stale_after_ns, 2'000 * 1'000'000LL);
  EXPECT_FALSE(parsed.value().workload.has_value());
}

TEST(CliStressTest, AVerifiedRunIsGivenTenSecondsToStopNotOneBecauseAUnitCannotBeInterrupted) {
  // CI, TSan x86, 2026-10-22: a unit took 1.5 s, the worker was mid-unit at the
  // deadline, and the default one-second grace killed it with SIGTERM.
  controller::RunOptions options;
  ASSERT_EQ(options.grace.stop_ns, 1'000'000'000) << "the default this test is about";
  allow_for_verified_units(options);
  EXPECT_EQ(options.grace.stop_ns, 10'000'000'000);
  EXPECT_GT(options.grace.stop_ns, 3'000'000'000) << "a worker 3 s into its unit is not killed";
  EXPECT_EQ(options.stale_after_ns, 10'000'000'000);
}

TEST(CliStressTest, AGraceAlreadyLongerThanTheVerifiedOneIsNotShortened) {
  controller::RunOptions options;
  options.grace.stop_ns = 30'000'000'000;
  options.stale_after_ns = 20'000'000'000;
  allow_for_verified_units(options);
  EXPECT_EQ(options.grace.stop_ns, 30'000'000'000);
  EXPECT_EQ(options.stale_after_ns, 20'000'000'000);
}

TEST(CliStressTest, ARunOfTheNullWorkloadIsStillTerminatedAfterOneSecond) {
  // The null workload never reaches allow_for_verified_units: the stale test
  // above shows its workers are given the two-second allowance, and the grace
  // is the controller's own default, which only a verifying run raises.
  const controller::RunOptions options;
  EXPECT_EQ(options.grace.stop_ns, 1'000'000'000);
  EXPECT_EQ(options.grace.kill_ns, 1'000'000'000);
}

TEST(CliStressTest, ARegisteredWorkloadThatIsSwitchedOffDoesNotMakeTheRunAVerifyingOne) {
  testing::FakeSyscalls syscalls;
  syscalls.advance_clocks_per_read(10'000'000);
  syscalls.script_forks({{0, 0}});
  syscalls.set_parent_pid(testing::FakeSyscalls::kFakePid);
  config::Config config = two_workloads();
  config.workloads[0] = config::Workload{"compression", config::Threads::count(1), false, {}};
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, config, "/opt/loadforge", one_cpu_machine(), supervisor_only(),
                       out, err),
            120);
  std::vector<std::string_view> tail;
  const std::vector<std::string>& argv = syscalls.last_exec_argv();
  for (std::size_t at = 2; at < argv.size(); ++at) {
    tail.emplace_back(argv[at]);
  }
  auto parsed = worker::parse_args(tail);
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value().pacing.stale_after_ns, 2'000 * 1'000'000LL)
      << "only an enabled registered workload is given the longer allowance";
}

TEST(CliStressTest, AMachineWhoseCacheLineIsNotKnownDoesNotRunAWorkload) {
  const controller_testing::MachineTree tree(
      controller_testing::MachineShape{2, std::nullopt, false});
  const topology::Snapshot machine = tree.accepted();
  testing::FakeSyscalls syscalls;
  syscalls.advance_clocks_per_read(10'000'000);
  config::Config config = two_workloads();
  config.workloads[0].name = "compression";
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(run_stress(syscalls, config, "/opt/loadforge", machine, supervisor_only(), out, err),
            1);
  EXPECT_NE(err.str().find("compression: the machine's cache-line size is not known"),
            std::string::npos)
      << err.str();
  EXPECT_EQ(syscalls.fork_count(), 0);
}

TEST(CliStressTest, TheCpusAWorkerMayUseAreWhatTheKernelPermitsOfWhatTheSnapshotFound) {
  const controller_testing::MachineTree tree(controller_testing::MachineShape{3, 64, false});
  const topology::Snapshot machine = tree.accepted();
  const auto cpus_of_first_worker = [&](testing::FakeSyscalls& syscalls) {
    syscalls.advance_clocks_per_read(10'000'000);
    syscalls.script_forks({{0, 0}});
    syscalls.set_parent_pid(testing::FakeSyscalls::kFakePid);
    config::Config config = two_workloads();
    config.workloads[0].name = "compression";
    config.workloads[0].threads = config::Threads::all();
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_stress(syscalls, config, "/opt/loadforge", machine, supervisor_only(), out, err),
              120);
    std::vector<std::string_view> tail;
    const std::vector<std::string>& argv = syscalls.last_exec_argv();
    for (std::size_t at = 2; at < argv.size(); ++at) {
      tail.emplace_back(argv[at]);
    }
    return worker::parse_args(tail).value().cpus;
  };
  {
    testing::FakeSyscalls syscalls;
    platform::CpuMask only_one_and_two;
    only_one_and_two.cpus.set(1);
    only_one_and_two.cpus.set(2);
    syscalls.permit_cpus(only_one_and_two);
    syscalls.set_current_affinity(only_one_and_two);
    EXPECT_EQ(cpus_of_first_worker(syscalls), (std::vector<std::uint32_t>{1, 2}));
  }
  {
    testing::FakeSyscalls syscalls;
    syscalls.fail_get_affinity(EIO);
    EXPECT_EQ(cpus_of_first_worker(syscalls), (std::vector<std::uint32_t>{0, 1, 2}))
        << "the kernel will not say: the snapshot's, and a placement refusal names any withheld";
  }
}

TEST(Cli, TheReRunSwitchIsInternalAndRefusesBadArguments) {
  const Outcome r = invoke({"--rerun", "--cpu", "0"});
  EXPECT_EQ(r.code, 22);
  EXPECT_EQ(r.err, "loadforge --rerun: --workload: required\n");
}

}  // namespace
}  // namespace loadforge::cli
