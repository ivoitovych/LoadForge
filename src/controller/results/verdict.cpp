// SPDX-License-Identifier: GPL-3.0-or-later
#include <variant>

#include "controller/results/aggregate.hpp"
#include "controller/results/result.hpp"
#include "controller/supervision.hpp"
#include "verification/record.hpp"

namespace loadforge::results {

Verdict derive_verdict(const RunResult& result) {
  const FindingCounts counts = count_findings(result);
  if (counts.wrong_results > 0 || counter_advanced(result.signature, kUncorrectedErrorsMetric)) {
    return Verdict::kFindings;
  }
  bool unfinished = false;
  for (const WorkerResult& worker : result.workers) {
    unfinished = unfinished || !controller::is_clean(worker.fate);
  }
  bool unchecked = false;
  for (const WorkloadResult& workload : result.workloads) {
    unchecked =
        unchecked || workload.tally.oracle_blocks == 0 || workload.tally.integrity_checked == 0;
  }
  if (result.workloads.empty() || counts.not_performed > 0 || unchecked || unfinished) {
    return Verdict::kIncomplete;
  }
  if (counter_advanced(result.signature, kCorrectedErrorsMetric)) {
    return Verdict::kCorrectedErrors;
  }
  return Verdict::kClean;
}

}  // namespace loadforge::results
