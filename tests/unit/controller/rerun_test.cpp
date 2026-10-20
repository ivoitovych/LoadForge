// SPDX-License-Identifier: GPL-3.0-or-later
//
// The re-runner: what it asks a re-run process to do (parsed back from the argv
// it execs), what it makes of how the process ended, and everything that can go
// wrong in starting and waiting for one.

#include "controller/rerun.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"
#include "support/fake_syscalls.hpp"
#include "verification/isolation.hpp"
#include "worker/entry.hpp"

namespace loadforge::controller {
namespace {

using loadforge::testing::FakeSyscalls;
using verification::RerunOutcome;
using verification::RerunRequest;
using verification::Rung;

constexpr pid_t kChild = 777;
/// With pipes numbered from here, the re-runner's pipe is (kReadFd, kReadFd + 1).
constexpr int kReadFd = 100;

WorkerWork a_work() {
  WorkerWork work;
  work.descriptor.name = "compression";
  work.descriptor.version = 1;
  work.isa = simd::IsaPath::kScalar;
  work.decomposition = 1;
  work.threads = 2;
  work.seed = 11;
  work.footprint_bytes = std::uint64_t{4} << 20U;
  work.line_bytes = 64;
  return work;
}

verification::Provenance a_provenance() {
  verification::Provenance where;
  where.logical_cpu = 3;
  where.workload = "compression";
  where.workload_version = 1;
  where.iteration = 42;
  where.seed = 99;
  where.isa = simd::IsaPath::kScalar;
  return where;
}

verification::WrongResult a_finding(const verification::Provenance& where = a_provenance()) {
  verification::ExactBitCorruption record;
  record.provenance = where;
  record.check = "decoded output against input";
  return record;
}

verification::WrongResult a_bounded_finding() {
  verification::NumericalVerificationFailure record;
  record.provenance = a_provenance();
  record.check = "residual 2-norm";
  record.residual = 1.0;
  record.tolerance = 0.5;
  return record;
}

RerunRequest a_request() { return RerunRequest{Rung::kOtherCore, 5, 2}; }

struct Rig {
  FakeSyscalls syscalls;
  RealRerunner rerunner;
  explicit Rig(std::int64_t timeout_ns = 1'000'000'000)
      : rerunner(syscalls, "/opt/loadforge", RerunOptions{timeout_ns}) {
    syscalls.number_pipes_from(kReadFd);
    syscalls.script_forks({{kChild, 0}});
    syscalls.set_parent_pid(FakeSyscalls::kFakePid);
    rerunner.use(a_work());
  }
  /// The child exits with `code` (a raw status is the code shifted a byte).
  void child_exits(int code) { syscalls.script_waits({{platform::Reaped{kChild, code << 8}, 0}}); }
};

TEST(RealRerunner, ACleanExitIsCleanAndTheReproductionCodeIsAReproduction) {
  {
    Rig rig;
    rig.child_exits(static_cast<int>(worker::RerunExit::kClean));
    EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kClean);
  }
  {
    Rig rig;
    rig.child_exits(static_cast<int>(worker::RerunExit::kReproduced));
    EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kReproduced);
  }
  {
    Rig rig;
    rig.child_exits(static_cast<int>(worker::RerunExit::kReproduced));
    EXPECT_EQ(rig.rerunner.rerun(a_bounded_finding(), a_request()), RerunOutcome::kReproduced)
        << "a bounded failure is repeated the same way";
  }
}

TEST(RealRerunner, EveryOtherEndingIsARunThatDidNotAnswerNeverAPass) {
  for (const int code : {static_cast<int>(worker::RerunExit::kNotPerformed),
                         static_cast<int>(worker::RerunExit::kBadArguments), 1, 2, 119, 124, 255}) {
    Rig rig;
    rig.child_exits(code);
    EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed) << code;
  }
  for (const int signal : {SIGSEGV, SIGABRT, SIGKILL, SIGBUS}) {
    Rig rig;
    rig.syscalls.script_waits({{platform::Reaped{kChild, signal}, 0}});
    EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed)
        << "a signal names a fault and not its cause: signal " << signal;
  }
}

TEST(RealRerunner, WithoutTheRunsFactsOrTheUnitsIdentityNothingIsStarted) {
  const auto not_started = [](Rig& rig, const verification::WrongResult& finding,
                              const RerunRequest& request) {
    EXPECT_EQ(rig.rerunner.rerun(finding, request), RerunOutcome::kNotPerformed);
    EXPECT_EQ(rig.syscalls.fork_count(), 0);
  };
  {
    FakeSyscalls syscalls;
    RealRerunner unused(syscalls, "/opt/loadforge", RerunOptions{});
    EXPECT_EQ(unused.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
    EXPECT_EQ(syscalls.fork_count(), 0) << "no run to repeat a unit of";
  }
  {
    Rig rig;
    not_started(rig, a_finding(), RerunRequest{Rung::kSameCore, 1, 0});
  }
  const auto without = [&](auto clear) {
    Rig rig;
    verification::Provenance where = a_provenance();
    clear(where);
    not_started(rig, a_finding(where), a_request());
  };
  without([](verification::Provenance& where) { where.workload.reset(); });
  without([](verification::Provenance& where) { where.workload_version.reset(); });
  without([](verification::Provenance& where) { where.iteration.reset(); });
  without([](verification::Provenance& where) { where.seed.reset(); });
  without([](verification::Provenance& where) { where.isa.reset(); });
  without([](verification::Provenance& where) { where.workload = "sort"; });
}

TEST(RealRerunner, TheProcessIsToldTheUnitTheRunsFactsAndTheRung) {
  Rig rig;
  rig.syscalls.fork_yields_child();
  rig.syscalls.script_forks({{0, 0}});  // This process is the child; its exec fails.
  EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
  EXPECT_EQ(rig.syscalls.last_exec_path(), "/opt/loadforge");
  const std::vector<std::string>& argv = rig.syscalls.last_exec_argv();
  ASSERT_GE(argv.size(), 2U);
  EXPECT_EQ(argv[1], worker::kRerunSwitch);
  std::vector<std::string_view> tail;
  for (std::size_t at = 2; at < argv.size(); ++at) {
    tail.emplace_back(argv[at]);
  }
  auto parsed = worker::parse_rerun_args(tail);
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  const worker::RerunArgs& args = parsed.value();
  EXPECT_EQ(args.unit.workload, "compression");
  EXPECT_EQ(args.unit.seed, 99U) << "the record's, not the run's";
  EXPECT_EQ(args.unit.footprint_bytes, std::uint64_t{4} << 20U) << "the run's, not the record's";
  EXPECT_EQ(args.unit.decomposition, 1U);
  EXPECT_EQ(args.unit.threads, 2U) << "the rung's";
  EXPECT_EQ(args.unit.isa, simd::IsaPath::kScalar);
  EXPECT_EQ(args.unit.line_bytes, 64U);
  EXPECT_EQ(args.workload_version, 1U);
  EXPECT_EQ(args.iteration, 42U);
  EXPECT_EQ(args.cpu, 5U) << "the rung's";
  EXPECT_EQ(args.unit.duty, (verification::DutyCycle{1, 1})) << "every check on";
}

TEST(RealRerunner, AChildWhoseExecFailedMustExitAndNothingIsStartedAfterIt) {
  Rig rig;
  rig.syscalls.script_forks({{0, 0}});
  EXPECT_FALSE(rig.rerunner.child_exit_code().has_value());
  EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
  ASSERT_TRUE(rig.rerunner.child_exit_code().has_value());
  EXPECT_EQ(*rig.rerunner.child_exit_code(),
            static_cast<int>(platform::WorkerStartupFailure::kExecFailed));
  const int forks = rig.syscalls.fork_count();
  EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
  EXPECT_EQ(rig.syscalls.fork_count(), forks) << "the child is not a controller";
}

TEST(RealRerunner, ARunThatCannotBeStartedIsNotPerformed) {
  {
    Rig rig;
    rig.syscalls.fail_create_pipe(EMFILE);
    EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
    EXPECT_EQ(rig.syscalls.fork_count(), 0);
  }
  {
    Rig rig;
    rig.syscalls.script_forks({{0, EAGAIN}});
    EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
    EXPECT_FALSE(rig.rerunner.child_exit_code().has_value());
  }
}

TEST(RealRerunner, ARunThatNeverEndsIsKilledAndNotPerformed) {
  Rig rig(std::int64_t{300} * 1'000'000);                         // Three looks of 100 ms.
  rig.syscalls.script_readiness(kReadFd, platform::Readiness{});  // Still going.
  rig.syscalls.script_waits({{platform::Reaped{kChild, SIGKILL}, 0}});
  EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
  EXPECT_EQ(rig.syscalls.poll_count(), 3);
  ASSERT_EQ(rig.syscalls.signals_sent().size(), 1U);
  EXPECT_EQ(rig.syscalls.signals_sent()[0], (std::pair<pid_t, int>{kChild, SIGKILL}));
}

TEST(RealRerunner, AFailedLookIsTheSameAsARunThatNeverEnds) {
  Rig rig;
  rig.syscalls.fail_poll(EBADF);
  rig.syscalls.script_waits({{platform::Reaped{kChild, SIGKILL}, 0}});
  EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
  EXPECT_EQ(rig.syscalls.signals_sent().size(), 1U) << "ended, as it could not be seen to end";
}

TEST(RealRerunner, ARunThatEndedButCannotBeReapedIsNotPerformedAndNotKilled) {
  Rig rig;
  rig.syscalls.script_waits({{platform::Reaped{}, ECHILD}});
  EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
  EXPECT_TRUE(rig.syscalls.signals_sent().empty());
}

TEST(RealRerunner, ARunThatEndedJustAsTheTimeWentIsStillAnAnswerOnlyIfItWasSeenToEnd) {
  // Out of looks with the process gone is the same observation as one that
  // never ended: a process nobody saw end has not answered.
  Rig rig(0);
  rig.child_exits(static_cast<int>(worker::RerunExit::kReproduced));
  EXPECT_EQ(rig.rerunner.rerun(a_finding(), a_request()), RerunOutcome::kNotPerformed);
}

}  // namespace
}  // namespace loadforge::controller
