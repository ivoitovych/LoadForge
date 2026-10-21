// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/isolation/ladder.hpp"

#include <cstdint>

#include "verification/isolation.hpp"

namespace loadforge::verification::isolation {

Answer answer_of(RerunOutcome outcome) noexcept {
  switch (outcome) {
    case RerunOutcome::kReproduced:
      return Answer::kYes;
    case RerunOutcome::kClean:
      return Answer::kNo;
    case RerunOutcome::kNotPerformed:
      return Answer::kUnknown;
  }
  return Answer::kUnknown;
}

IsolationReport LadderIsolator::isolate(const WrongResult& finding, const Placement& placement,
                                        Rerunner& rerunner) {
  IsolationReport report;
  const auto climb = [&](Rung rung, std::uint32_t cpu, std::uint32_t threads) {
    const RerunRequest request{rung, cpu, threads};
    const RerunOutcome outcome = rerunner.rerun(finding, request);
    report.steps.push_back(Step{request, outcome});
    return answer_of(outcome);
  };

  const bool one_thread = placement.threads == 1;
  report.answers.same_core = climb(Rung::kSameCore, placement.cpu, placement.threads);
  if (one_thread) {
    report.answers.single_thread = report.answers.same_core;
  }
  if (report.answers.same_core == Answer::kYes) {
    if (!placement.other_cores.empty()) {
      report.answers.other_core =
          climb(Rung::kOtherCore, placement.other_cores.front(), placement.threads);
    }
    if (!one_thread) {
      report.answers.single_thread = climb(Rung::kSingleThread, placement.cpu, 1);
    }
  }
  report.finding = derive_finding(report.answers);
  report.confidence = confidence_of(report.finding);
  return report;
}

}  // namespace loadforge::verification::isolation
