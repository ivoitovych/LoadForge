// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/isolate.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "controller/report.hpp"
#include "controller/rerun.hpp"
#include "topology/snapshot.hpp"
#include "verification/isolation.hpp"
#include "verification/isolation/placement.hpp"
#include "verification/record.hpp"

namespace loadforge::controller {

IsolationSummary isolate_findings(RunReport& report, const topology::Snapshot* machine,
                                  verification::Isolator& isolator, ChildAwareRerunner& rerunner,
                                  std::size_t limit) {
  IsolationSummary summary;
  for (WorkerReport& worker : report.workers) {
    if (!worker.work.has_value()) {
      continue;
    }
    rerunner.use(*worker.work);
    std::size_t isolated_here = 0;
    for (FindingReport& finding : worker.findings) {
      const std::optional<verification::WrongResult> wrong =
          verification::as_wrong_result(finding.record);
      if (!wrong.has_value()) {
        finding.not_isolated = "a check that was not made: there is no wrong result to repeat";
        continue;
      }
      if (isolated_here >= limit) {
        finding.not_isolated = "past the limit of " + std::to_string(limit) + " isolations";
        continue;
      }
      const std::optional<std::uint32_t> cpu =
          verification::provenance_of(finding.record).logical_cpu;
      if (!cpu.has_value()) {
        finding.not_isolated =
            "the worker ran on several CPUs, so the record names none to repeat it on";
        continue;
      }
      if (machine == nullptr) {
        finding.not_isolated = "no topology snapshot to name another core from";
        continue;
      }
      const verification::Placement placement =
          verification::isolation::placement_for(*machine, *cpu, worker.work->threads);
      finding.isolation = isolator.isolate(*wrong, placement, rerunner);
      ++isolated_here;
      ++summary.isolated;
      if (const auto child = rerunner.child_exit_code(); child.has_value()) {
        summary.child_exit_code = child;
        return summary;
      }
    }
  }
  return summary;
}

}  // namespace loadforge::controller
