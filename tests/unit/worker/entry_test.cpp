// SPDX-License-Identifier: GPL-3.0-or-later
//
// The worker entry point: the internal switch's arguments, parsed and
// refused by name; the sequence from attach to exit code; and what reaches
// the record pipe on each way out.

#include "worker/entry.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/process.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"

namespace loadforge::worker {
namespace {

using loadforge::ipc::HeartbeatRegion;
using loadforge::platform::PipeEnds;
using loadforge::platform::WorkerStartupFailure;
using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kMs = 1'000'000;

// The worker's codes and the launcher's must never collide: both reach the
// supervisor through one byte.
static_assert(static_cast<int>(WorkerExit::kSetupFailed) <
              static_cast<int>(WorkerStartupFailure::kExecFailed));
static_assert(static_cast<int>(WorkerExit::kControllerLost) >
              static_cast<int>(WorkerStartupFailure::kDescriptorNotInherited));
static_assert(static_cast<int>(WorkerExit::kBadArguments) < 126);

std::vector<std::string_view> views_of(const std::vector<std::string>& strings) {
  std::vector<std::string_view> views;
  views.reserve(strings.size());
  for (const std::string& s : strings) {
    views.emplace_back(s);
  }
  return views;
}

WorkerArgs typical() {
  WorkerArgs args;
  args.slot = 1;
  args.workers = 3;
  args.heartbeat_fd = 100;
  args.record_fd = 201;
  args.control_fd = 202;
  args.supervisor_pid = 4000;
  args.pacing = Pacing{5000 * kMs, 50 * kMs, 500 * kMs};
  return args;
}

/// The argv the controller would exec, minus the executable and the switch.
std::vector<std::string> tail_of(const WorkerArgs& args) {
  std::vector<std::string> argv = argv_for("/usr/bin/loadforge", args);
  argv.erase(argv.begin(), argv.begin() + 2);
  return argv;
}

std::string refusal_of(const std::vector<std::string>& argv) {
  auto parsed = parse_args(views_of(argv));
  EXPECT_FALSE(parsed.has_value());
  return parsed.has_value() ? std::string{"(accepted)"} : parsed.error();
}

/// `argv` with the value after `key` replaced.
std::vector<std::string> with(std::vector<std::string> argv, std::string_view key,
                              std::string value) {
  for (std::size_t index = 0; index + 1 < argv.size(); index += 2) {
    if (argv[index] == key) {
      argv[index + 1] = std::move(value);
      return argv;
    }
  }
  argv.emplace_back(key);
  argv.push_back(std::move(value));
  return argv;
}

std::vector<std::string> without(std::vector<std::string> argv, std::string_view key) {
  for (std::size_t index = 0; index + 1 < argv.size(); index += 2) {
    if (argv[index] == key) {
      argv.erase(argv.begin() + static_cast<std::ptrdiff_t>(index),
                 argv.begin() + static_cast<std::ptrdiff_t>(index) + 2);
      break;
    }
  }
  return argv;
}

// --- arguments ------------------------------------------------------------------

TEST(WorkerArgsTest, TheArgvForAWorkerParsesBackToTheSameArguments) {
  const WorkerArgs args = typical();
  const std::vector<std::string> argv = argv_for("/usr/bin/loadforge", args);
  ASSERT_GE(argv.size(), 2U);
  EXPECT_EQ(argv[0], "/usr/bin/loadforge");
  EXPECT_EQ(argv[1], kWorkerSwitch);
  auto parsed = parse_args(views_of(tail_of(args)));
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value(), args);

  WorkerArgs pinned = args;
  pinned.cpus = {0, 3, 17};
  auto with_cpus = parse_args(views_of(tail_of(pinned)));
  ASSERT_TRUE(with_cpus.has_value()) << with_cpus.error();
  EXPECT_EQ(with_cpus.value(), pinned);
  EXPECT_EQ(tail_of(pinned).back(), "0,3,17");
}

TEST(WorkerArgsTest, EveryRequiredKeyIsRequiredAndNamedWhenMissing) {
  const std::vector<std::string> argv = tail_of(typical());
  for (const std::string_view key :
       {"--slot", "--workers", "--heartbeat-fd", "--record-fd", "--control-fd", "--supervisor-pid",
        "--duration-ms", "--interval-ms", "--stale-after-ms"}) {
    EXPECT_EQ(refusal_of(without(argv, key)), std::string{key} + ": required");
  }
  EXPECT_EQ(refusal_of({}), "--slot: required");
}

TEST(WorkerArgsTest, ShapeFaultsAreRefusedByKey) {
  const std::vector<std::string> argv = tail_of(typical());
  EXPECT_EQ(refusal_of(with(argv, "--bogus", "1")), "--bogus: not a worker argument");
  std::vector<std::string> dangling = argv;
  dangling.emplace_back("--cpus");
  EXPECT_EQ(refusal_of(dangling), "--cpus: missing its value");
  std::vector<std::string> twice = argv;
  twice.emplace_back("--slot");
  twice.emplace_back("1");
  EXPECT_EQ(refusal_of(twice), "--slot: given twice");
}

TEST(WorkerArgsTest, EveryValueHasADomainAndTheBoundaryIsRefusedByKey) {
  const std::vector<std::string> argv = tail_of(typical());
  EXPECT_EQ(refusal_of(with(argv, "--workers", "abc")),
            "--workers: expected a whole non-negative number, got 'abc'");
  EXPECT_EQ(refusal_of(with(argv, "--workers", "-1")),
            "--workers: expected a whole non-negative number, got '-1'");
  EXPECT_EQ(refusal_of(with(argv, "--workers", "3ms")),
            "--workers: expected a whole non-negative number, got '3ms'");
  EXPECT_EQ(refusal_of(with(argv, "--workers", "0")), "--workers: must be at least 1");
  EXPECT_EQ(refusal_of(with(argv, "--workers", "1025")),
            "--workers: 1025 is past the limit of 1024");
  EXPECT_EQ(refusal_of(with(argv, "--slot", "3")), "--slot: 3 is past the limit of 2");
  EXPECT_EQ(refusal_of(with(argv, "--record-fd", "2147483648")),
            "--record-fd: 2147483648 is past the limit of 2147483647");
  EXPECT_EQ(refusal_of(with(argv, "--supervisor-pid", "0")),
            "--supervisor-pid: must be at least 1");
  EXPECT_EQ(refusal_of(with(argv, "--supervisor-pid", "init")),
            "--supervisor-pid: expected a whole non-negative number, got 'init'");
  EXPECT_EQ(refusal_of(with(argv, "--duration-ms", "0")), "--duration-ms: must be at least 1");
  EXPECT_EQ(refusal_of(with(argv, "--interval-ms", "0")), "--interval-ms: must be at least 1");
  EXPECT_EQ(refusal_of(with(argv, "--stale-after-ms", "49")),
            "--stale-after-ms: must be at least the interval");
  // One millisecond past what fits in nanoseconds.
  EXPECT_EQ(refusal_of(with(argv, "--duration-ms", "9223372036855")),
            "--duration-ms: 9223372036855 is past the limit of 9223372036854");
  auto largest = parse_args(views_of(with(argv, "--duration-ms", "9223372036854")));
  ASSERT_TRUE(largest.has_value()) << largest.error();
  EXPECT_EQ(largest.value().pacing.duration_ns, 9223372036854000000);

  EXPECT_EQ(refusal_of(with(argv, "--cpus", "")),
            "--cpus: expected a whole non-negative number, got ''");
  EXPECT_EQ(refusal_of(with(argv, "--cpus", "1,,2")),
            "--cpus: expected a whole non-negative number, got ''");
  EXPECT_EQ(refusal_of(with(argv, "--cpus", "1,")),
            "--cpus: expected a whole non-negative number, got ''");
  EXPECT_EQ(refusal_of(with(argv, "--cpus", "4294967296")),
            "--cpus: 4294967296 is past the limit of 4294967295");
  auto one = parse_args(views_of(with(argv, "--cpus", "7")));
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(one.value().cpus, (std::vector<std::uint32_t>{7}));
}

TEST(WorkerArgsTest, EqualityIsFieldByField) {
  const WorkerArgs a = typical();
  WorkerArgs b = a;
  EXPECT_EQ(a, b);
  b.slot = 0;
  EXPECT_NE(a, b);
  b = a;
  b.workers = 9;
  EXPECT_NE(a, b);
  b = a;
  b.heartbeat_fd = 9;
  EXPECT_NE(a, b);
  b = a;
  b.record_fd = 9;
  EXPECT_NE(a, b);
  b = a;
  b.control_fd = 9;
  EXPECT_NE(a, b);
  b = a;
  b.supervisor_pid = 9;
  EXPECT_NE(a, b);
  b = a;
  b.pacing.interval_ns = 9;
  EXPECT_NE(a, b);
  b = a;
  b.cpus = {1};
  EXPECT_NE(a, b);
}

// --- the run --------------------------------------------------------------------

/// A controller's side of a worker, over the fake: a region for two workers
/// with the controller's beat published, a record pipe, a control pipe, and
/// the argv a controller would exec the worker with.
struct Rig {
  FakeSyscalls syscalls;
  std::optional<HeartbeatRegion> region;
  PipeEnds records;
  PipeEnds control;
  std::ostringstream err;

  Rig() {
    auto created = HeartbeatRegion::create(syscalls, 2);
    EXPECT_TRUE(created.has_value());
    region.emplace(std::move(created).value());
    EXPECT_TRUE(region->publish(ipc::kControllerSlot, ipc::Heartbeat{0, 1, 1, 1}).has_value());
    auto record_pipe = syscalls.create_pipe();
    EXPECT_TRUE(record_pipe.has_value());
    records = record_pipe.value();
    auto control_pipe = syscalls.create_pipe();
    EXPECT_TRUE(control_pipe.has_value());
    control = control_pipe.value();
    syscalls.advance_clocks_per_read(10 * kMs);
  }

  WorkerArgs args() {
    WorkerArgs args;
    args.slot = 0;
    args.workers = 2;
    args.heartbeat_fd = region->fd();
    args.record_fd = records.write_fd;
    args.control_fd = control.read_fd;
    args.supervisor_pid = FakeSyscalls::kFakeSupervisorPid;
    args.pacing = Pacing{30 * kMs, 10 * kMs, 100 * kMs};
    return args;
  }

  int run(const WorkerArgs& args) {
    const std::vector<std::string> argv = tail_of(args);
    return worker::run(syscalls, views_of(argv), err);
  }

  /// The records the worker wrote, as (type, text) pairs.
  std::vector<std::pair<std::uint16_t, std::string>> received() const {
    std::vector<std::pair<std::uint16_t, std::string>> out;
    ipc::RecordReader reader;
    const std::string bytes = syscalls.pipe_contents(records.write_fd);
    std::vector<std::byte> raw;
    for (const char c : bytes) {
      raw.push_back(static_cast<std::byte>(c));
    }
    reader.feed(raw);
    while (true) {
      auto next = reader.next();
      EXPECT_TRUE(next.has_value());
      if (!next.has_value() || !next.value().has_value()) {
        break;
      }
      std::string text;
      for (const std::byte b : next.value()->payload) {
        text.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
      }
      out.emplace_back(next.value()->type, text);
    }
    EXPECT_TRUE(reader.finish().has_value());
    return out;
  }
};

constexpr std::uint16_t kStarted = static_cast<std::uint16_t>(ipc::RecordType::kStarted);
constexpr std::uint16_t kFinished = static_cast<std::uint16_t>(ipc::RecordType::kFinished);
constexpr std::uint16_t kError = static_cast<std::uint16_t>(ipc::RecordType::kError);

TEST(WorkerRunTest, RunsToCompletionReportingStartAndFinishOnThePipe) {
  Rig rig;
  WorkerArgs args = rig.args();
  args.cpus = {0};
  EXPECT_EQ(rig.run(args), static_cast<int>(WorkerExit::kCompleted));
  EXPECT_EQ(rig.err.str(), "");
  EXPECT_EQ(rig.syscalls.signals_ignored(), (std::vector<int>{SIGPIPE}));
  EXPECT_EQ(rig.received(), (std::vector<std::pair<std::uint16_t, std::string>>{
                                {kStarted, "slot 0 pid 4242"},
                                {kFinished, "the duration elapsed after 2 tick(s), 30 ms"}}));
  // Placed where asked, and the beats landed in the worker's slot.
  EXPECT_EQ(rig.syscalls.affinity_pids(), (std::vector<pid_t>{0}));
  EXPECT_EQ(rig.syscalls.current_affinity().cpus.count(), 1U);
  auto beat = rig.region->observe(ipc::worker_slot(0));
  ASSERT_TRUE(beat.has_value() && beat.value().has_value());
  EXPECT_EQ(beat.value()->progress, 2U);
  EXPECT_EQ(beat.value()->pid, 4242U);
}

TEST(WorkerRunTest, BadArgumentsAreRefusedBeforeAnythingIsTouched) {
  Rig rig;
  const std::vector<std::string_view> argv{"--slot", "0"};
  EXPECT_EQ(worker::run(rig.syscalls, argv, rig.err), static_cast<int>(WorkerExit::kBadArguments));
  EXPECT_EQ(rig.err.str(), "loadforge --worker: --workers: required\n");
  EXPECT_TRUE(rig.syscalls.signals_ignored().empty());
  EXPECT_EQ(rig.syscalls.write_count(), 0);
}

TEST(WorkerRunTest, SetupFailuresExitWithTheirOwnCodeAndSayWhich) {
  {
    Rig rig;
    rig.syscalls.fail_ignore_signal(EINVAL);
    EXPECT_EQ(rig.run(rig.args()), static_cast<int>(WorkerExit::kSetupFailed));
    EXPECT_EQ(rig.err.str(),
              "loadforge --worker: sigaction(signal 13): Invalid argument (errno 22)\n");
    EXPECT_EQ(rig.syscalls.write_count(), 0) << "no channel yet";
  }
  {
    Rig rig;
    WorkerArgs args = rig.args();
    args.heartbeat_fd = 7;  // Not a descriptor this process holds.
    EXPECT_EQ(rig.run(args), static_cast<int>(WorkerExit::kSetupFailed));
    EXPECT_EQ(rig.err.str(),
              "loadforge --worker: heartbeat region: mmap(fd 7, 256 bytes): Bad file "
              "descriptor (errno 9)\n");
    EXPECT_EQ(rig.syscalls.write_count(), 0);
  }
  {
    Rig rig;
    rig.syscalls.script_writes({{0, EIO}});  // The started record cannot be sent.
    EXPECT_EQ(rig.run(rig.args()), static_cast<int>(WorkerExit::kSetupFailed));
    EXPECT_EQ(rig.err.str(),
              "loadforge --worker: record channel: write(fd 201): Input/output error (errno 5)\n");
    EXPECT_EQ(rig.syscalls.write_count(), 1) << "the error is not sent down a dead channel";
  }
  {
    Rig rig;
    WorkerArgs args = rig.args();
    args.cpus = {4096};  // Parses; no mask can name it.
    EXPECT_EQ(rig.run(args), static_cast<int>(WorkerExit::kSetupFailed));
    EXPECT_NE(rig.err.str().find("loadforge --worker: cpus: "), std::string::npos) << rig.err.str();
    const auto records = rig.received();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].first, kStarted);
    EXPECT_EQ(records[1].first, kError);
    EXPECT_EQ(records[1].second.substr(0, 6), "cpus: ");
  }
  {
    Rig rig;
    platform::CpuMask only_zero;
    only_zero.cpus.set(0);
    rig.syscalls.permit_cpus(only_zero);
    WorkerArgs args = rig.args();
    args.cpus = {1};
    EXPECT_EQ(rig.run(args), static_cast<int>(WorkerExit::kSetupFailed));
    EXPECT_NE(rig.err.str().find("cpus: "), std::string::npos) << rig.err.str();
    EXPECT_NE(rig.err.str().find("no CPU in the mask"), std::string::npos) << rig.err.str();
    EXPECT_EQ(rig.received().back().first, kError);
  }
  {
    Rig rig;
    rig.syscalls.fail_poll(EBADF);
    EXPECT_EQ(rig.run(rig.args()), static_cast<int>(WorkerExit::kSetupFailed));
    EXPECT_EQ(rig.err.str(),
              "loadforge --worker: control pipe: poll(1 descriptors): Bad file descriptor "
              "(errno 9)\n");
    const auto records = rig.received();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[1], (std::pair<std::uint16_t, std::string>{
                              kError,
                              "control pipe: poll(1 descriptors): Bad file descriptor "
                              "(errno 9)"}));
  }
  {
    Rig rig;
    rig.syscalls.script_writes({{4096, 0}, {0, EIO}});  // Started goes; finished does not.
    EXPECT_EQ(rig.run(rig.args()), static_cast<int>(WorkerExit::kSetupFailed));
    EXPECT_EQ(rig.err.str(),
              "loadforge --worker: record channel: write(fd 201): Input/output error (errno 5)\n");
    EXPECT_EQ(rig.syscalls.write_count(), 2);
  }
}

TEST(WorkerRunTest, ARecordPipeWithNoReaderIsTheControllerLostNotASetupFailure) {
  // EPIPE is the one refusal that says whose fault it is: nothing holds the
  // read end, so the controller is gone. The controller-killed supervision
  // test met it for real, a controller killed before a worker's started
  // record, and the worker called it its own setup failure (119).
  {
    Rig rig;
    ASSERT_TRUE(rig.syscalls.close(rig.records.read_fd).has_value());  // Nobody listening.
    EXPECT_EQ(rig.run(rig.args()), static_cast<int>(WorkerExit::kControllerLost));
    EXPECT_EQ(rig.err.str(),
              "loadforge --worker: record channel: write(fd 201): Broken pipe (errno 32)\n");
    EXPECT_EQ(rig.syscalls.write_count(), 1) << "the error is not sent down a dead channel";
  }
  {
    Rig rig;
    rig.syscalls.script_writes({{4096, 0}, {0, EPIPE}});  // Started goes; finished does not.
    EXPECT_EQ(rig.run(rig.args()), static_cast<int>(WorkerExit::kControllerLost));
    EXPECT_EQ(rig.err.str(),
              "loadforge --worker: record channel: write(fd 201): Broken pipe (errno 32)\n");
    EXPECT_EQ(rig.syscalls.write_count(), 2);
  }
}

TEST(WorkerRunTest, AControllerThatIsGoneIsItsOwnExitCodeNotACompletion) {
  Rig rig;
  ASSERT_TRUE(rig.syscalls.close(rig.control.write_fd).has_value());
  EXPECT_EQ(rig.run(rig.args()), static_cast<int>(WorkerExit::kControllerLost));
  EXPECT_EQ(rig.err.str(),
            "loadforge --worker: controller lost: the controller's control pipe hung up after 0 "
            "tick(s), 10 ms\n");
  EXPECT_EQ(rig.received(), (std::vector<std::pair<std::uint16_t, std::string>>{
                                {kStarted, "slot 0 pid 4242"},
                                {kError,
                                 "controller lost: the controller's control pipe hung up after 0 "
                                 "tick(s), 10 ms"}}));
}

TEST(WorkerRunTest, AStopRequestIsACompletion) {
  Rig rig;
  rig.syscalls.deliver_to_pipe(rig.control.read_fd, "x");
  EXPECT_EQ(rig.run(rig.args()), static_cast<int>(WorkerExit::kCompleted));
  EXPECT_EQ(rig.received().back(),
            (std::pair<std::uint16_t, std::string>{
                kFinished, "the controller asked for a stop after 0 tick(s), 10 ms"}));
}

}  // namespace
}  // namespace loadforge::worker
