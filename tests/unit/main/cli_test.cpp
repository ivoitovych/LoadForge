// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/clock.hpp"
#include "platform/syscalls.hpp"
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
  std::vector<std::string_view> views(argv.begin(), argv.end());
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

TEST(Cli, DiagnosticsGoToStderrNotStdout) {
  // A caller piping --version into a script must never receive an error message
  // on the same stream.
  const Outcome r = invoke({"--nope"});
  EXPECT_TRUE(r.out.empty());
  EXPECT_FALSE(r.err.empty());
}

}  // namespace
}  // namespace loadforge::cli
