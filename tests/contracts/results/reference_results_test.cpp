// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference verdict, run against its suite, and the definitions
// the results header carries.

#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>

#include "contracts/results/results_contract.hpp"
#include "controller/results/result.hpp"

namespace loadforge::contracts {
namespace {

using results::RunResult;
using results::Verdict;

bool counter_rose(const RunResult& run, std::string_view name) {
  for (const statistics::MetricSummary& summary : run.signature.metrics) {
    const auto* counter = std::get_if<statistics::CounterSummary>(&summary.value);
    if (summary.metric.name == name && counter != nullptr && counter->delta > 0) {
      return true;
    }
  }
  return false;
}

Verdict reference_verdict(const RunResult& run) {
  bool unchecked = run.workloads.empty();
  for (const results::WorkloadResult& workload : run.workloads) {
    for (const results::Finding& finding : workload.findings) {
      if (verification::is_wrong_result(finding.record)) {
        return Verdict::kFindings;
      }
      unchecked = true;
    }
    unchecked =
        unchecked || workload.tally.oracle_blocks == 0 || workload.tally.integrity_checked == 0;
  }
  if (counter_rose(run, results::kUncorrectedErrorsMetric)) {
    return Verdict::kFindings;
  }
  for (const results::WorkerResult& worker : run.workers) {
    unchecked = unchecked || !controller::is_clean(worker.fate);
  }
  if (unchecked) {
    return Verdict::kIncomplete;
  }
  return counter_rose(run, results::kCorrectedErrorsMetric) ? Verdict::kCorrectedErrors
                                                            : Verdict::kClean;
}

struct ReferenceVerdict {
  static Verdict derive(const RunResult& run) { return reference_verdict(run); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, VerdictContract, ::testing::Types<ReferenceVerdict>);

TEST(ResultsDefinition, VerdictWordsAndTheSchemaVersion) {
  EXPECT_EQ(results::describe(Verdict::kClean), "clean");
  EXPECT_EQ(results::describe(Verdict::kCorrectedErrors), "corrected errors");
  EXPECT_EQ(results::describe(Verdict::kIncomplete), "incomplete");
  EXPECT_EQ(results::describe(Verdict::kFindings), "findings");
  EXPECT_EQ(results::describe(static_cast<Verdict>(9)), "unrecognised verdict");
  EXPECT_EQ(results::kSchemaVersion, 1U);
  EXPECT_EQ(RunResult{}.schema_version, results::kSchemaVersion);
  EXPECT_EQ(RunResult{}.verdict, Verdict::kIncomplete) << "nothing is clean until shown to be";
}

}  // namespace
}  // namespace loadforge::contracts
