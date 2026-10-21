// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/outcome.hpp"

#include <algorithm>
#include <functional>
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

int exit_code_for(results::Verdict verdict, bool workers_clean, bool verified) noexcept {
  if (verdict == results::Verdict::kFindings || verdict == results::Verdict::kCorrectedErrors) {
    return kExitFault;
  }
  if (!workers_clean) {
    return kExitUnclean;
  }
  return verified && verdict == results::Verdict::kIncomplete ? kExitNotVerified : 0;
}

bool has_verified_worker(const controller::RunReport& report) noexcept {
  return std::ranges::any_of(report.workers, [](const controller::WorkerReport& worker) {
    return worker.work.has_value();
  });
}

int with_output_failure(int code) noexcept {
  return code == kExitFault ? kExitFault : kExitOutputFailed;
}

int exit_code_for(const controller::RunReport& report, const config::Config& config) {
  const bool verified = has_verified_worker(report);
  results::Verdict verdict = results::Verdict::kClean;
  if (verified) {
    verdict = to_run_result(report, config).verdict;
  }
  return exit_code_for(verdict, controller::all_clean(report), verified);
}

int after_isolation(controller::RunReport& report, const topology::Snapshot* machine,
                    verification::Isolator& isolator, controller::ChildAwareRerunner& rerunner,
                    std::size_t limit, controller::Reporter& reporter,
                    const std::function<int()>& conclude) {
  if (controller::wrong_result_count(report) != 0) {
    const controller::IsolationSummary isolated =
        controller::isolate_findings(report, machine, isolator, rerunner, limit);
    if (isolated.child_exit_code.has_value()) {
      return *isolated.child_exit_code;
    }
    reporter.isolation_finished(report);
  }
  return conclude();
}

int finish_run(controller::RunReport& report, const config::Config& config,
               const topology::Snapshot* machine, verification::Isolator& isolator,
               controller::ChildAwareRerunner& rerunner, std::size_t limit,
               controller::Reporter& reporter) {
  return after_isolation(report, machine, isolator, rerunner, limit, reporter,
                         [&report, &config] { return exit_code_for(report, config); });
}

}  // namespace loadforge::cli
