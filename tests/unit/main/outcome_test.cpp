// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a finished run is to the shell: the controller's report in the results'
// types, and the exit code their verdict gives.

#include "main/outcome.hpp"

#include <gtest/gtest.h>

#include <csignal>

#include <optional>
#include <vector>

#include "controller/isolate.hpp"
#include "controller/report.hpp"
#include "controller/rerun.hpp"
#include "controller/supervision.hpp"
#include "unit/controller/consumer_models.hpp"
#include "unit/controller/machine_tree.hpp"

namespace loadforge::cli {
namespace {

using controller::FindingReport;
using controller::WorkerFate;
using controller::WorkerReport;

config::Config a_config() {
  config::Config config;
  config.run.mode = config::Mode::kStress;
  return config;
}

WorkerReport null_worker(WorkerFate fate = WorkerFate::kCompleted) {
  WorkerReport worker;
  worker.fate = fate;
  return worker;
}

WorkerReport checked_worker(WorkerFate fate = WorkerFate::kCompleted) {
  WorkerReport worker;
  worker.fate = fate;
  controller::WorkerWork work;
  work.descriptor.name = "compression";
  work.cpu = 3;
  work.logical_bytes = 1000;
  worker.work = work;
  worker.tally_seen = true;
  worker.tally.units_computed = 5;
  worker.tally.oracle_checked = 5;
  worker.tally.oracle_blocks = 5;
  worker.tally.integrity_checked = 5;
  return worker;
}

FindingReport wrong_result() {
  verification::ExactBitCorruption record;
  record.check = "decoded output against input";
  FindingReport finding;
  finding.record = record;
  return finding;
}

FindingReport unrun_check() {
  verification::VerificationNotPerformed record;
  record.what = "w";
  record.why = "y";
  FindingReport finding;
  finding.record = record;
  return finding;
}

int code_of(std::vector<WorkerReport> workers) {
  controller::RunReport report;
  report.workers = std::move(workers);
  return exit_code_for(report, a_config());
}

TEST(ExitCode, ARunOfNullWorkloadsIsItsWorkersAlone) {
  EXPECT_EQ(code_of({null_worker(), null_worker()}), 0);
  EXPECT_EQ(code_of({null_worker(), null_worker(WorkerFate::kFault)}), kExitUnclean);
}

TEST(ExitCode, ARunThatCheckedAndFoundNothingIsClean) {
  EXPECT_EQ(code_of({checked_worker(), checked_worker()}), 0);
}

TEST(ExitCode, AWrongResultOutranksEverythingElse) {
  WorkerReport found = checked_worker();
  found.findings = {wrong_result()};
  found.wrong_results = 1;
  EXPECT_EQ(code_of({found, checked_worker()}), kExitWrongResult);
  EXPECT_EQ(code_of({found, checked_worker(WorkerFate::kFault)}), kExitWrongResult)
      << "the finding is the most important thing the run has to say";
}

TEST(ExitCode, ACheckThatWasNotMadeIsNeitherACleanRunNorAFinding) {
  WorkerReport unchecked = checked_worker();
  unchecked.findings = {unrun_check()};
  EXPECT_EQ(code_of({unchecked, checked_worker()}), kExitNotVerified);
  WorkerReport blind = checked_worker();
  blind.tally.oracle_blocks = 0;
  EXPECT_EQ(code_of({blind}), kExitNotVerified) << "an oracle that compared nothing";
  WorkerReport unbracketed = checked_worker();
  unbracketed.tally.integrity_checked = 0;
  EXPECT_EQ(code_of({unbracketed}), kExitNotVerified);
  WorkerReport silent = checked_worker();
  silent.tally_seen = false;
  silent.tally = {};
  EXPECT_EQ(code_of({silent}), kExitNotVerified) << "a worker that never reported a tally";
}

TEST(ExitCode, AWorkerThatDidNotCompleteIsUncleanEvenWhenNothingElseIsWrong) {
  EXPECT_EQ(code_of({checked_worker(), checked_worker(WorkerFate::kKilled)}), kExitUnclean);
  WorkerReport unchecked = checked_worker(WorkerFate::kKilled);
  unchecked.findings = {unrun_check()};
  EXPECT_EQ(code_of({unchecked}), kExitUnclean) << "unfinished outranks unchecked";
}

TEST(ExitCode, AnIncompleteRunWithAnUnfinishedWorkerIsUncleanWhateverElseIsMissing) {
  WorkerReport blind = checked_worker(WorkerFate::kFault);
  blind.tally.oracle_blocks = 0;
  EXPECT_EQ(code_of({blind}), kExitUnclean);
}

class Counting final : public controller::Reporter {
 public:
  void run_starting(const controller::RunPlan& /*plan*/) override {}
  void event(std::int64_t /*elapsed_ns*/, std::string_view /*line*/) override {}
  void run_finished(const controller::RunReport& /*report*/) override {}
  void isolation_finished(const controller::RunReport& /*report*/) override { ++isolations; }
  int isolations = 0;
};

class Rerunner final : public controller::ChildAwareRerunner {
 public:
  void use(const controller::WorkerWork& /*work*/) override {}
  [[nodiscard]] verification::RerunOutcome rerun(
      const verification::WrongResult& /*finding*/,
      const verification::RerunRequest& /*request*/) override {
    ++asked;
    return verification::RerunOutcome::kClean;
  }
  [[nodiscard]] std::optional<int> child_exit_code() const noexcept override { return child_exit; }
  std::optional<int> child_exit;
  int asked = 0;
};

TEST(FinishRun, NothingWrongMeansNothingIsRepeatedAndNobodyIsToldAndTheCodeIsTheRuns) {
  const controller_testing::MachineTree tree(controller_testing::MachineShape{4, 64, false});
  const topology::Snapshot machine = tree.accepted();
  controller_testing::ModelIsolator isolator;
  Rerunner rerunner;
  Counting reporter;
  controller::RunReport report;
  report.workers = {checked_worker()};
  EXPECT_EQ(finish_run(report, a_config(), &machine, isolator, rerunner, 5, reporter), 0);
  EXPECT_EQ(rerunner.asked, 0);
  EXPECT_EQ(reporter.isolations, 0);
}

controller::RunReport one_wrong_result() {
  WorkerReport found = checked_worker();
  FindingReport finding = wrong_result();
  std::get<verification::ExactBitCorruption>(finding.record).provenance.logical_cpu = 1;
  found.findings = {finding};
  found.wrong_results = 1;
  controller::RunReport report;
  report.workers = {found};
  return report;
}

TEST(FinishRun, AWrongResultIsRepeatedThenTheReporterIsToldAndTheCodeIsTheFindings) {
  const controller_testing::MachineTree tree(controller_testing::MachineShape{4, 64, false});
  const topology::Snapshot machine = tree.accepted();
  controller_testing::ModelIsolator isolator;
  Rerunner rerunner;
  Counting reporter;
  controller::RunReport report = one_wrong_result();
  EXPECT_EQ(finish_run(report, a_config(), &machine, isolator, rerunner, 5, reporter),
            kExitWrongResult);
  EXPECT_GT(rerunner.asked, 0);
  EXPECT_EQ(reporter.isolations, 1);
  EXPECT_TRUE(report.workers[0].findings[0].isolation.has_value());
}

TEST(FinishRun, ARerunWhoseExecFailedHereMustExitWithItsCodeBeforeAnythingIsReported) {
  const controller_testing::MachineTree tree(controller_testing::MachineShape{4, 64, false});
  const topology::Snapshot machine = tree.accepted();
  controller_testing::ModelIsolator isolator;
  Rerunner rerunner;
  rerunner.child_exit = 126;
  Counting reporter;
  controller::RunReport report = one_wrong_result();
  EXPECT_EQ(finish_run(report, a_config(), &machine, isolator, rerunner, 5, reporter), 126);
  EXPECT_EQ(reporter.isolations, 0) << "this process is a failed child, not the controller";
}

TEST(ToRunResult, OneWorkloadResultPerVerifyingWorkerAndOneWorkerResultPerWorker) {
  controller::RunReport report;
  report.elapsed_ns = 123;
  WorkerReport found = checked_worker();
  found.slot = 1;
  found.termination = "exited with code 0";
  found.findings = {wrong_result(), unrun_check()};
  report.workers = {null_worker(), found};
  const results::RunResult result = to_run_result(report, a_config());
  EXPECT_EQ(result.mode, config::Mode::kStress);
  EXPECT_EQ(result.elapsed_ns, 123);
  ASSERT_EQ(result.workers.size(), 2U);
  EXPECT_FALSE(result.workers[0].cpu.has_value());
  EXPECT_EQ(result.workers[1].slot, 1U);
  EXPECT_EQ(result.workers[1].cpu, 3U);
  EXPECT_EQ(result.workers[1].termination, "exited with code 0");
  ASSERT_EQ(result.workloads.size(), 1U) << "the null workload is not a workload result";
  const results::WorkloadResult& workload = result.workloads[0];
  EXPECT_EQ(workload.descriptor.name, "compression");
  EXPECT_EQ(workload.tally.units_computed, 5U);
  EXPECT_EQ(workload.logical_bytes, 5000U);
  EXPECT_EQ(workload.lock_policy, found.work->lock);
  EXPECT_FALSE(workload.memory_lock.has_value()) << "the worker said nothing";
  ASSERT_EQ(workload.findings.size(), 2U);
  EXPECT_EQ(result.verdict, results::Verdict::kFindings);
  EXPECT_EQ(result.schema_version, results::kSchemaVersion);

  // What the worker's runner said about locking is carried into its workload's result.
  report.workers[1].memory = verification::MemoryLock{
      verification::LockPolicy::kTry, verification::LockOutcome::kFellBack, "refused"};
  const results::RunResult told = to_run_result(report, a_config());
  ASSERT_TRUE(told.workloads[0].memory_lock.has_value());
  EXPECT_EQ(told.workloads[0].memory_lock->detail, "refused");
}

// ---- the codes M2 and M3 each assigned, composed ---------------------------------------------

TEST(ExitCodeComposed, TheCodesAreMainsAndTheOnesM2AddedFollowThem) {
  EXPECT_EQ(kExitUnclean, 3);
  EXPECT_EQ(kExitOutputFailed, 4);
  EXPECT_EQ(kExitFault, 5);
  EXPECT_EQ(kExitWrongResult, kExitFault) << "a wrong result is a fault, the verdict is findings";
  EXPECT_EQ(kExitNotVerified, 6);
  EXPECT_EQ(kExitTemperature, 7);
}

// ---- the temperature ceiling: 5, then 7, then 4, 3, 6 ------------------------------------------

controller::TemperatureStop reached() {
  controller::TemperatureStop stop;
  stop.sensor = "thermal.package 0 (hwmon:coretemp)";
  stop.celsius = 91.0;
  stop.ceiling_c = 90;
  stop.at_ns = 12'345'000'000;
  return stop;
}

TEST(ExitCodeTemperature, ARunTheCeilingEndedIsSevenWhateverElseItsStopCaused) {
  // The stop kills workers that did not finish (3) and cuts the checks short (6): 7 says why.
  EXPECT_EQ(exit_code_for(results::Verdict::kClean, true, true, true), kExitTemperature);
  EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, true, true, true), kExitTemperature);
  EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, false, true, true), kExitTemperature);
  EXPECT_EQ(exit_code_for(results::Verdict::kClean, false, false, true), kExitTemperature);
}

TEST(ExitCodeTemperature, OnlyAFaultOutranksIt) {
  for (const bool clean : {true, false}) {
    EXPECT_EQ(exit_code_for(results::Verdict::kFindings, clean, true, true), kExitFault);
    EXPECT_EQ(exit_code_for(results::Verdict::kCorrectedErrors, clean, true, true), kExitFault);
  }
}

TEST(ExitCodeTemperature, AResultThatCannotBeWrittenDoesNotHideIt) {
  EXPECT_EQ(with_output_failure(kExitTemperature), kExitTemperature);
  EXPECT_EQ(with_output_failure(kExitUnclean), kExitOutputFailed);
}

TEST(ExitCodeTemperature, TheReportsStopIsWhatMakesItSeven) {
  controller::RunReport report;
  report.workers = {checked_worker(), checked_worker()};
  EXPECT_EQ(exit_code_for(report, a_config()), 0);
  report.temperature_stop = reached();
  EXPECT_EQ(exit_code_for(report, a_config()), kExitTemperature);
  report.workers[0].findings = {wrong_result()};
  report.workers[0].wrong_results = 1;
  EXPECT_EQ(exit_code_for(report, a_config()), kExitFault) << "a wrong result found on the way";
}

TEST(ToRunResult, EveryWorkersAccountOfItsEndNamesTheCeilingThatEndedTheRun) {
  controller::RunReport report;
  WorkerReport first = null_worker();
  first.termination = "exited with code 0";
  WorkerReport second = null_worker(WorkerFate::kStoppedByController);
  second.slot = 1;
  second.termination = "killed by signal 9";
  report.workers = {first, second};
  EXPECT_EQ(to_run_result(report, a_config()).workers[0].termination, "exited with code 0")
      << "no stop, no words";
  report.temperature_stop = reached();
  const results::RunResult result = to_run_result(report, a_config());
  EXPECT_EQ(result.workers[0].termination,
            "exited with code 0; the run was ended by the temperature ceiling: "
            "thermal.package 0 (hwmon:coretemp) read 91.0 C, at or above the ceiling of 90 C, "
            "12.345 s into the run");
  EXPECT_EQ(result.workers[1].termination.find("killed by signal 9; the run was ended by"), 0U);
}

// ---- a signal: 5, 7, 4, then 130 or 143, then 3, 6 ---------------------------------------------

TEST(ExitCodeSignal, ARunASignalStoppedIsTheShellsCodeForThatSignal) {
  EXPECT_EQ(kExitSignalBase, 128);
  EXPECT_EQ(exit_code_for(results::Verdict::kClean, true, true, false, SIGINT), 130);
  EXPECT_EQ(exit_code_for(results::Verdict::kClean, true, true, false, SIGTERM), 143);
}

TEST(ExitCodeSignal, ItExplainsTheWorkersItHadToKillAndTheChecksItCutShort) {
  for (const int signal : {SIGINT, SIGTERM}) {
    EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, false, true, false, signal),
              kExitSignalBase + signal);
    EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, true, true, false, signal),
              kExitSignalBase + signal);
    EXPECT_EQ(exit_code_for(results::Verdict::kClean, false, false, false, signal),
              kExitSignalBase + signal);
  }
}

TEST(ExitCodeSignal, AFaultAndTheCeilingOutrankItAndNothingElseDoes) {
  for (const bool clean : {true, false}) {
    EXPECT_EQ(exit_code_for(results::Verdict::kFindings, clean, true, false, SIGINT), kExitFault);
    EXPECT_EQ(exit_code_for(results::Verdict::kCorrectedErrors, clean, true, true, SIGINT),
              kExitFault);
    EXPECT_EQ(exit_code_for(results::Verdict::kClean, clean, true, true, SIGINT), kExitTemperature);
  }
}

TEST(ExitCodeSignal, AResultThatCannotBeWrittenOutranksItButNotAFaultOrTheCeiling) {
  EXPECT_EQ(with_output_failure(kExitSignalBase + SIGINT), kExitOutputFailed);
  EXPECT_EQ(with_output_failure(kExitSignalBase + SIGTERM), kExitOutputFailed);
  EXPECT_EQ(with_output_failure(kExitFault), kExitFault);
  EXPECT_EQ(with_output_failure(kExitTemperature), kExitTemperature);
}

TEST(ExitCodeSignal, TheReportsSignalIsWhatMakesIt) {
  controller::RunReport report;
  report.workers = {checked_worker(), checked_worker()};
  EXPECT_EQ(exit_code_for(report, a_config()), 0);
  report.stop_signal = SIGINT;
  EXPECT_EQ(exit_code_for(report, a_config()), 130);
  report.stop_signal = SIGTERM;
  EXPECT_EQ(exit_code_for(report, a_config()), 143);
  report.temperature_stop = reached();
  EXPECT_EQ(exit_code_for(report, a_config()), kExitTemperature);
  report.temperature_stop.reset();
  report.workers[0].findings = {wrong_result()};
  report.workers[0].wrong_results = 1;
  EXPECT_EQ(exit_code_for(report, a_config()), kExitFault) << "a wrong result found on the way";
}

TEST(ToRunResult, EveryWorkersAccountOfItsEndNamesTheSignalThatStoppedTheRun) {
  controller::RunReport report;
  WorkerReport first = null_worker();
  first.termination = "exited with code 0";
  report.workers = {first};
  report.stop_signal = SIGINT;
  EXPECT_EQ(to_run_result(report, a_config()).workers[0].termination,
            "exited with code 0; the run was stopped by SIGINT");
  report.stop_forced = true;
  EXPECT_EQ(to_run_result(report, a_config()).workers[0].termination,
            "exited with code 0; the run was stopped by SIGINT; a later signal killed the "
            "workers at once");
}

TEST(ExitCodeComposed, AFaultOutranksAWorkerThatDidNotCompleteAndAnIncompleteVerdict) {
  for (const bool clean : {true, false}) {
    for (const bool verified : {true, false}) {
      EXPECT_EQ(exit_code_for(results::Verdict::kFindings, clean, verified), kExitFault);
      EXPECT_EQ(exit_code_for(results::Verdict::kCorrectedErrors, clean, verified), kExitFault);
    }
  }
}

TEST(ExitCodeComposed, AWorkerThatDidNotCompleteOutranksACheckThatWasNotMade) {
  EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, false, true), kExitUnclean);
  EXPECT_EQ(exit_code_for(results::Verdict::kClean, false, true), kExitUnclean);
  EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, true, true), kExitNotVerified);
}

TEST(ExitCodeComposed, WithNothingToVerifyAnIncompleteVerdictIsWhatSuchARunAlwaysHas) {
  EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, true), 0);
  EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, true, false), 0);
  EXPECT_EQ(exit_code_for(results::Verdict::kIncomplete, false), kExitUnclean);
  EXPECT_EQ(exit_code_for(results::Verdict::kClean, true, true), 0);
}

TEST(ExitCodeComposed, AResultThatCannotBeWrittenIsFourUnlessAFaultIsTheNews) {
  EXPECT_EQ(with_output_failure(0), kExitOutputFailed);
  EXPECT_EQ(with_output_failure(kExitUnclean), kExitOutputFailed);
  EXPECT_EQ(with_output_failure(kExitNotVerified), kExitOutputFailed);
  EXPECT_EQ(with_output_failure(kExitFault), kExitFault);
}

TEST(ExitCodeComposed, ARunHasAVerifiedWorkerWhenAnyWorkerRanOne) {
  controller::RunReport none;
  none.workers = {null_worker()};
  EXPECT_FALSE(has_verified_worker(none));
  controller::RunReport some;
  some.workers = {null_worker(), checked_worker()};
  EXPECT_TRUE(has_verified_worker(some));
}

}  // namespace
}  // namespace loadforge::cli
