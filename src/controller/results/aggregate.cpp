// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/results/aggregate.hpp"

#include <limits>
#include <variant>

#include "verification/record.hpp"

namespace loadforge::results {

FindingCounts count_findings(const RunResult& result) {
  FindingCounts counts;
  for (const WorkloadResult& workload : result.workloads) {
    for (const Finding& finding : workload.findings) {
      if (verification::is_wrong_result(finding.record)) {
        ++counts.wrong_results;
      } else {
        ++counts.not_performed;
      }
    }
  }
  return counts;
}

verification::Tally total_tally(const std::vector<WorkloadResult>& workloads) {
  verification::Tally total;
  for (const WorkloadResult& workload : workloads) {
    total = total + workload.tally;
  }
  return total;
}

std::uint64_t logical_bytes(std::uint64_t units_computed, std::uint64_t bytes_per_unit) noexcept {
  std::uint64_t product = 0;
  if (__builtin_mul_overflow(units_computed, bytes_per_unit, &product)) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return product;
}

bool counter_advanced(const statistics::LoadSignature& signature, std::string_view name) {
  for (const statistics::MetricSummary& summary : signature.metrics) {
    if (summary.metric.name != name) {
      continue;
    }
    const auto* counter = std::get_if<statistics::CounterSummary>(&summary.value);
    if (counter != nullptr && counter->delta > 0) {
      return true;
    }
  }
  return false;
}

void seal(RunResult& result) {
  result.schema_version = kSchemaVersion;
  result.verdict = derive_verdict(result);
}

}  // namespace loadforge::results
