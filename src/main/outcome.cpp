// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/outcome.hpp"

#include <algorithm>
#include <utility>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "controller/results/aggregate.hpp"
#include "controller/results/result.hpp"

namespace loadforge::cli {

results::RunResult to_run_result(const controller::RunReport& report,
                                 const config::Config& config) {
  results::RunResult result;
  result.mode = config.run.mode;
  result.elapsed_ns = report.elapsed_ns;
  for (const controller::WorkerReport& worker : report.workers) {
    results::WorkerResult entry;
    entry.slot = worker.slot;
    entry.fate = worker.fate;
    if (worker.work.has_value()) {
      entry.cpu = worker.work->cpu;
    }
    entry.termination = worker.termination;
    result.workers.push_back(std::move(entry));
    if (!worker.work.has_value()) {
      continue;
    }
    results::WorkloadResult workload;
    workload.descriptor = worker.work->descriptor;
    workload.isa = worker.work->isa;
    workload.decomposition = worker.work->decomposition;
    workload.duty = worker.work->duty;
    workload.tally = worker.tally;
    workload.logical_bytes =
        results::logical_bytes(worker.tally.units_computed, worker.work->logical_bytes);
    for (const controller::FindingReport& finding : worker.findings) {
      results::Finding kept;
      kept.record = finding.record;
      kept.isolation = finding.isolation;
      workload.findings.push_back(std::move(kept));
    }
    result.workloads.push_back(std::move(workload));
  }
  results::seal(result);
  return result;
}

int exit_code_for(const controller::RunReport& report, const config::Config& config) {
  const bool verified = std::ranges::any_of(
      report.workers,
      [](const controller::WorkerReport& worker) { return worker.work.has_value(); });
  if (!verified) {
    return controller::all_clean(report) ? 0 : kExitUnclean;
  }
  const results::Verdict verdict = to_run_result(report, config).verdict;
  if (verdict == results::Verdict::kFindings) {
    return kExitWrongResult;
  }
  if (verdict == results::Verdict::kIncomplete) {
    return controller::all_clean(report) ? kExitNotVerified : kExitUnclean;
  }
  return 0;  // Clean, or clean with corrected errors.
}

int finish_run(controller::RunReport& report, const config::Config& config,
               const topology::Snapshot* machine, verification::Isolator& isolator,
               controller::ChildAwareRerunner& rerunner, std::size_t limit,
               controller::Reporter& reporter) {
  if (controller::wrong_result_count(report) != 0) {
    const controller::IsolationSummary isolated =
        controller::isolate_findings(report, machine, isolator, rerunner, limit);
    if (isolated.child_exit_code.has_value()) {
      return *isolated.child_exit_code;
    }
    reporter.isolation_finished(report);
  }
  return exit_code_for(report, config);
}

}  // namespace loadforge::cli
