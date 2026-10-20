// SPDX-License-Identifier: GPL-3.0-or-later
//
// derive_verdict() against its contract suite, and the corners the suite does
// not name: the rules' order, and what a series that is not a counter, or not
// the named one, does not do.

#include <gtest/gtest.h>

#include <string>

#include "contracts/results/results_contract.hpp"
#include "controller/results/result.hpp"

namespace loadforge::results {
namespace {

namespace suite = contracts::results_suite;

struct DerivedVerdict {
  static Verdict derive(const RunResult& result) { return derive_verdict(result); }
};

TEST(VerdictTest, ADefaultResultIsIncompleteNeverClean) {
  const RunResult empty;
  EXPECT_EQ(empty.verdict, Verdict::kIncomplete);
  EXPECT_EQ(derive_verdict(empty), Verdict::kIncomplete);
  EXPECT_EQ(empty.schema_version, kSchemaVersion);
}

TEST(VerdictTest, EveryVerdictHasItsOwnWords) {
  EXPECT_EQ(describe(Verdict::kClean), "clean");
  EXPECT_EQ(describe(Verdict::kCorrectedErrors), "corrected errors");
  EXPECT_EQ(describe(Verdict::kIncomplete), "incomplete");
  EXPECT_EQ(describe(Verdict::kFindings), "findings");
}

TEST(VerdictTest, AGaugeOfTheSameNameIsNotAnError) {
  RunResult run = suite::clean_run();
  const statistics::MetricSummary gauge{
      suite::counter(std::string(kUncorrectedErrorsMetric)),
      statistics::GaugeSummary{3, 1.0, 9.0, 5.0, 1.0, 1.0, 9.0, 0.5}, 0};
  run.signature.metrics.push_back(gauge);
  EXPECT_EQ(derive_verdict(run), Verdict::kClean);
}

TEST(VerdictTest, AnotherCountersMovementIsNotAnError) {
  RunResult run = suite::clean_run();
  run.signature.metrics.push_back(suite::counted("edac.something_else", 7));
  EXPECT_EQ(derive_verdict(run), Verdict::kClean);
}

TEST(VerdictTest, OneMovingSeriesAmongManyIsEnough) {
  RunResult run = suite::clean_run();
  run.signature.metrics.push_back(suite::counted(std::string(kUncorrectedErrorsMetric), 0));
  run.signature.metrics.push_back(suite::counted(std::string(kUncorrectedErrorsMetric), 0));
  run.signature.metrics.push_back(suite::counted(std::string(kUncorrectedErrorsMetric), 2));
  EXPECT_EQ(derive_verdict(run), Verdict::kFindings);
}

TEST(VerdictTest, ASecondUnfinishedWorkerAndASecondUncheckedWorkloadChangeNothing) {
  RunResult run = suite::clean_run();
  run.workers.push_back({1, controller::WorkerFate::kKilled, std::nullopt, "killed by signal 9"});
  run.workers.push_back({2, controller::WorkerFate::kOomKilled, std::nullopt, "oom"});
  run.workloads.push_back(run.workloads[0]);
  run.workloads[0].tally.oracle_blocks = 0;
  run.workloads[1].tally.integrity_checked = 0;
  EXPECT_EQ(derive_verdict(run), Verdict::kIncomplete);
}

TEST(VerdictTest, EveryWorkerFateButCompletedIsUnfinished) {
  for (const controller::WorkerFate fate :
       {controller::WorkerFate::kWorkerError, controller::WorkerFate::kStartupFailed,
        controller::WorkerFate::kControllerLostReported, controller::WorkerFate::kFault,
        controller::WorkerFate::kKilled, controller::WorkerFate::kOomKilled,
        controller::WorkerFate::kStoppedByController, controller::WorkerFate::kTimedOut,
        controller::WorkerFate::kChannelCorrupt, controller::WorkerFate::kTruncatedEvidence,
        controller::WorkerFate::kUnrecognised}) {
    RunResult run = suite::clean_run();
    run.workers[0].fate = fate;
    EXPECT_EQ(derive_verdict(run), Verdict::kIncomplete) << controller::describe(fate);
  }
}

}  // namespace
}  // namespace loadforge::results

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(Results, VerdictContract, ::testing::Types<results::DerivedVerdict>);
}  // namespace loadforge::contracts
