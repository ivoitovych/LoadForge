// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a finished run is to the shell: the controller's report in the results'
// types, and the exit code their verdict gives.

#include "main/outcome.hpp"

#include <gtest/gtest.h>

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
  ASSERT_EQ(workload.findings.size(), 2U);
  EXPECT_EQ(result.verdict, results::Verdict::kFindings);
  EXPECT_EQ(result.schema_version, results::kSchemaVersion);
}

}  // namespace
}  // namespace loadforge::cli
