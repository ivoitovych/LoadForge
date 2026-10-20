// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a finished run is to the shell: the controller's report in the results'
// types, and the exit code their verdict gives.

#include "main/outcome.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "controller/report.hpp"
#include "controller/supervision.hpp"

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
