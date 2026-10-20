// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/results/aggregate.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "contracts/results/results_contract.hpp"

namespace loadforge::results {
namespace {

namespace suite = contracts::results_suite;

TEST(AggregateTest, FindingsAreCountedByWhatTheySay) {
  RunResult run = suite::clean_run();
  EXPECT_EQ(count_findings(run).wrong_results, 0U);
  EXPECT_EQ(count_findings(run).not_performed, 0U);
  run.workloads.push_back(run.workloads[0]);
  run.workloads[0].findings = {suite::finding(suite::exact_record()),
                               suite::finding(suite::unrun_record())};
  run.workloads[1].findings = {
      suite::finding(suite::numerical_record()), suite::finding(suite::exact_record()),
      suite::finding(suite::unrun_record()), suite::finding(suite::unrun_record())};
  EXPECT_EQ(count_findings(run).wrong_results, 3U);
  EXPECT_EQ(count_findings(run).not_performed, 3U);
}

TEST(AggregateTest, TheTotalTallyIsTheSumFieldByField) {
  EXPECT_EQ(total_tally({}), verification::Tally{});
  std::vector<WorkloadResult> workloads(2);
  workloads[0].tally = verification::Tally{1, 2, 3, 4, 5, 6, 7};
  workloads[1].tally = verification::Tally{10, 20, 30, 40, 50, 60, 70};
  EXPECT_EQ(total_tally(workloads), (verification::Tally{11, 22, 33, 44, 55, 66, 77}));
}

TEST(AggregateTest, LogicalBytesAreTheProductAndSaturateAtTheCeiling) {
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(logical_bytes(0, 4096), 0U);
  EXPECT_EQ(logical_bytes(100, 8192), 819200U);
  EXPECT_EQ(logical_bytes(kMax, 1), kMax);
  EXPECT_EQ(logical_bytes(1, kMax), kMax);
  EXPECT_EQ(logical_bytes(kMax / 2 + 1, 2), kMax) << "2^64 does not fit, and is not 0";
  EXPECT_EQ(logical_bytes(kMax, 2), kMax);
  EXPECT_EQ(logical_bytes(kMax / 3, 3), kMax / 3 * 3) << "the largest product that fits";
}

TEST(AggregateTest, ACounterAdvancedOnlyWhenItIsTheNamedCounterAndItMoved) {
  statistics::LoadSignature signature;
  EXPECT_FALSE(counter_advanced(signature, "edac.corrected"));
  signature.metrics.push_back(suite::counted("edac.corrected", 0));
  EXPECT_FALSE(counter_advanced(signature, "edac.corrected"));
  signature.metrics.push_back(suite::counted("edac.other", 5));
  EXPECT_FALSE(counter_advanced(signature, "edac.corrected"));
  signature.metrics.push_back(
      {suite::counter("edac.corrected"),
       telemetry::Unavailable{topology::Availability::kDenied, "p", "d", "r"}, 0});
  EXPECT_FALSE(counter_advanced(signature, "edac.corrected"));
  signature.metrics.push_back(suite::counted("edac.corrected", 1));
  EXPECT_TRUE(counter_advanced(signature, "edac.corrected"));
}

TEST(AggregateTest, SealingStatesTheVersionAndTheVerdictTheContentsImply) {
  RunResult run = suite::clean_run();
  run.schema_version = 99;
  run.verdict = Verdict::kFindings;
  seal(run);
  EXPECT_EQ(run.schema_version, kSchemaVersion);
  EXPECT_EQ(run.verdict, Verdict::kClean);
  run.workers[0].fate = controller::WorkerFate::kKilled;
  seal(run);
  EXPECT_EQ(run.verdict, Verdict::kIncomplete);
}

}  // namespace
}  // namespace loadforge::results
