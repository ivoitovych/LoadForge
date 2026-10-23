// SPDX-License-Identifier: GPL-3.0-or-later
//
// `loadforge stress` and a signal (journal 34b): SIGINT and SIGTERM stop the run, which is
// reported, written and journalled as any run that returned, and exits with the shell's code
// for the signal. Over the fake, where the signal is a byte the test puts in the pipe the
// handler would have written to, from the poll the run waits in.

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "controller/journal/file_ops.hpp"
#include "main/cli.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/main/cli_support.hpp"
#include "unit/main/memory_files.hpp"

namespace loadforge::cli {
namespace {

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

/// A sink that keeps what it is given, and refuses on request.
class KeepingSink final : public ResultSink {
 public:
  core::Result<core::Ok, std::string> prepare(const std::string& /*directory*/) override {
    return core::Ok{};
  }
  core::Result<core::Ok, std::string> store(const std::string& path,
                                            const std::string& bytes) override {
    if (refuse_store) {
      return "the disk is full at " + path;
    }
    stored.emplace_back(path, bytes);
    return core::Ok{};
  }
  bool refuse_store = false;
  std::vector<std::pair<std::string, std::string>> stored;
};

/// Two workers that exit at once, nothing sent, with a signal raised at the run's first wait.
class StressSignal : public ::testing::Test {
 protected:
  void SetUp() override {
    syscalls.advance_clocks_per_read(10'000'000);
    syscalls.script_forks({{1001, 0}, {1002, 0}});
    syscalls.script_nonblocking_reaps(
        {{platform::Reaped{1001, 0}, 0}, {platform::Reaped{1002, 0}, 0}});
    options.sample = false;
    options.capabilities = false;
    options.sink = &sink;
    options.host = HostFacts{};
    options.formats = {"json"};
    options.output_dir = "results";
    options.journal_path = "run.lfj";
    options.journal_ops = &files;
    options.journal_files = &files;
  }

  void raise(int signal) {
    syscalls.on_poll([this, signal](int) {
      if (!raised) {
        raised = syscalls.raise_diverted(signal);
      }
    });
  }

  int run() {
    return run_stress(syscalls, two_workloads(), "/opt/loadforge", one_cpu_machine(), options, out,
                      err);
  }

  testing::FakeSyscalls syscalls;
  KeepingSink sink;
  cli_testing::MemoryFiles files;
  StressOptions options;
  std::ostringstream out;
  std::ostringstream err;
  bool raised = false;
};

TEST_F(StressSignal, SigintStopsTheRunWhichIsReportedWrittenAndJournalledAndExits130) {
  raise(SIGINT);

  EXPECT_EQ(run(), 130);

  EXPECT_TRUE(raised);
  EXPECT_TRUE(contains(out.str(), "SIGINT received: stop requested")) << out.str();
  EXPECT_TRUE(contains(out.str(), "  stopped by SIGINT\n")) << "the console report: " << out.str();
  EXPECT_TRUE(contains(out.str(), "the run was stopped by SIGINT: its exit code is 130"))
      << out.str();
  ASSERT_FALSE(sink.stored.empty()) << "the result file is written for a run that was stopped";
  EXPECT_TRUE(contains(sink.stored.front().second, "the run was stopped by SIGINT"))
      << "and says so";
  const auto shown = read_journal(files, files, "run.lfj");
  ASSERT_TRUE(shown.has_value()) << shown.error();
  EXPECT_EQ(shown.value().ending, journal::Ending::kTerminated) << "a clean end";
  EXPECT_TRUE(syscalls.signals_diverted().empty()) << "the default actions are back";
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

TEST_F(StressSignal, SigtermStopsItTooAndExits143) {
  raise(SIGTERM);

  EXPECT_EQ(run(), 143);

  EXPECT_TRUE(contains(out.str(), "SIGTERM received: stop requested")) << out.str();
  EXPECT_TRUE(contains(out.str(), "its exit code is 143")) << out.str();
}

TEST_F(StressSignal, AResultThatCannotBeWrittenOutranksTheSignalsCode) {
  sink.refuse_store = true;
  raise(SIGINT);

  EXPECT_EQ(run(), 4);

  EXPECT_TRUE(contains(err.str(), "loadforge: cannot write the result: ")) << err.str();
}

TEST_F(StressSignal, ARunNobodyInterruptsIsUnchangedAndLeavesTheSignalsAsItFoundThem) {
  EXPECT_EQ(run(), 3) << "the workers exited at once with nothing sent, as in the run without it";

  EXPECT_FALSE(raised);
  EXPECT_FALSE(contains(out.str(), "SIG"));
  EXPECT_TRUE(syscalls.signals_diverted().empty());
  EXPECT_EQ(syscalls.signals_defaulted(), (std::vector<int>{SIGCHLD, SIGINT, SIGTERM}))
      << "put back when the run was over, before anything else was done";
}

class StressSignalPipeFails : public StressSignal, public ::testing::WithParamInterface<int> {};

TEST_P(StressSignalPipeFails, TheRunIsRefusedBeforeAForkAndTheJournalEndsCleanly) {
  syscalls.fail_create_pipe(GetParam());

  EXPECT_EQ(run(), 1);

  EXPECT_TRUE(contains(err.str(), "loadforge: cannot catch SIGINT and SIGTERM: ")) << err.str();
  EXPECT_TRUE(contains(err.str(), "pipe2")) << err.str();
  EXPECT_EQ(syscalls.fork_count(), 0);
  const auto shown = read_journal(files, files, "run.lfj");
  ASSERT_TRUE(shown.has_value()) << shown.error();
  EXPECT_EQ(shown.value().ending, journal::Ending::kTerminated)
      << "the run never started; its journal does not read as a crash";
}

INSTANTIATE_TEST_SUITE_P(Pipe2Errors, StressSignalPipeFails,
                         ::testing::Values(EFAULT, EINVAL, EMFILE, ENFILE));

class StressSignalDivertFails : public StressSignal,
                                public ::testing::WithParamInterface<std::pair<int, int>> {};

TEST_P(StressSignalDivertFails, TheRunIsRefusedBeforeAForkAndNothingStaysCaught) {
  syscalls.fail_divert_signal_on(GetParam().first, GetParam().second);

  EXPECT_EQ(run(), 1);

  EXPECT_TRUE(contains(err.str(), "loadforge: cannot catch SIGINT and SIGTERM: ")) << err.str();
  EXPECT_TRUE(contains(err.str(), "sigaction")) << err.str();
  EXPECT_EQ(syscalls.fork_count(), 0);
  EXPECT_TRUE(syscalls.signals_diverted().empty());
  EXPECT_TRUE(syscalls.all_descriptors_closed());
}

INSTANTIATE_TEST_SUITE_P(SigactionErrors, StressSignalDivertFails,
                         ::testing::Values(std::pair{1, EFAULT}, std::pair{1, EINVAL},
                                           std::pair{2, EFAULT}, std::pair{2, EINVAL}));

}  // namespace
}  // namespace loadforge::cli
