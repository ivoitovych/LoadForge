// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_VERIFICATION_ISOLATION_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_VERIFICATION_ISOLATION_CONTRACT_HPP

// Provider-side suite for src/verification/isolation.hpp (ADR 0003, 0004).
//
// IsolatorContract<Subject>: Subject::make() returns std::unique_ptr<Isolator>.
//
// The suite brings a scripted Rerunner that answers by rung and records every
// request, so each path through the ladder is a test and each request is
// checked against the rung that made it.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <vector>

#include "verification/isolation.hpp"

namespace loadforge::contracts {

namespace isolation_suite {

using verification::RerunOutcome;
using verification::Rung;

class ScriptedRerunner final : public verification::Rerunner {
 public:
  explicit ScriptedRerunner(std::map<Rung, RerunOutcome> answers) : answers_(std::move(answers)) {}
  [[nodiscard]] RerunOutcome rerun(const verification::WrongResult& /*finding*/,
                                   const verification::RerunRequest& request) override {
    requests.push_back(request);
    const auto found = answers_.find(request.rung);
    return found == answers_.end() ? RerunOutcome::kNotPerformed : found->second;
  }
  std::vector<verification::RerunRequest> requests;

 private:
  std::map<Rung, RerunOutcome> answers_;
};

inline verification::WrongResult exact_finding() {
  verification::ExactBitCorruption record;
  record.provenance.logical_cpu = 5;
  record.provenance.iteration = 77;
  record.check = "c";
  return record;
}

inline verification::WrongResult numerical_finding() {
  verification::NumericalVerificationFailure record;
  record.provenance.logical_cpu = 5;
  record.check = "n";
  record.residual = 2.0;
  record.tolerance = 1.0;
  return record;
}

inline const verification::Placement kFourThreads{5, 4, {9, 11}};

}  // namespace isolation_suite

template <typename Subject>
class IsolatorContract : public ::testing::Test {
 protected:
  static verification::IsolationReport run(
      isolation_suite::ScriptedRerunner& rerunner,
      const verification::Placement& placement = isolation_suite::kFourThreads) {
    const auto isolator = Subject::make();
    const verification::IsolationReport report =
        isolator->isolate(isolation_suite::exact_finding(), placement, rerunner);
    EXPECT_EQ(report.finding, verification::derive_finding(report.answers));
    EXPECT_EQ(report.confidence, verification::confidence_of(report.finding));
    EXPECT_EQ(report.steps.size(), rerunner.requests.size());
    for (std::size_t step = 0; step < report.steps.size(); ++step) {
      EXPECT_EQ(report.steps[step].request, rerunner.requests[step]);
    }
    return report;
  }
};
TYPED_TEST_SUITE_P(IsolatorContract);

using isolation_suite::ScriptedRerunner;
using verification::Answer;
using verification::Finding;
using verification::RerunOutcome;
using verification::RerunRequest;
using verification::Rung;

TYPED_TEST_P(IsolatorContract, CleanOnTheSameCoreIsATransientAndStops) {
  ScriptedRerunner rerunner({{Rung::kSameCore, RerunOutcome::kClean}});
  const auto report = this->run(rerunner);
  EXPECT_EQ(rerunner.requests, (std::vector<RerunRequest>{{Rung::kSameCore, 5, 4}}));
  EXPECT_EQ(report.answers.same_core, Answer::kNo);
  EXPECT_EQ(report.finding, Finding::kNonReproducibleTransient);
  EXPECT_EQ(report.confidence, verification::Confidence::kLow);
}

TYPED_TEST_P(IsolatorContract, ASameCoreRerunThatCannotBeMadeIsInconclusiveAndStops) {
  ScriptedRerunner rerunner({{Rung::kSameCore, RerunOutcome::kNotPerformed}});
  const auto report = this->run(rerunner);
  EXPECT_EQ(rerunner.requests.size(), 1U);
  EXPECT_EQ(report.answers.same_core, Answer::kUnknown);
  EXPECT_EQ(report.finding, Finding::kInconclusive)
      << "never a transient: a re-run that did not happen did not come out clean";
}

TYPED_TEST_P(IsolatorContract, ReproducedEverywhereIsNotCoreLocalWithHighConfidence) {
  ScriptedRerunner rerunner({{Rung::kSameCore, RerunOutcome::kReproduced},
                             {Rung::kOtherCore, RerunOutcome::kReproduced},
                             {Rung::kSingleThread, RerunOutcome::kReproduced}});
  const auto report = this->run(rerunner);
  EXPECT_EQ(rerunner.requests,
            (std::vector<RerunRequest>{
                {Rung::kSameCore, 5, 4}, {Rung::kOtherCore, 9, 4}, {Rung::kSingleThread, 5, 1}}));
  EXPECT_EQ(report.finding, Finding::kNotCoreLocal);
  EXPECT_EQ(report.confidence, verification::Confidence::kHigh);
}

TYPED_TEST_P(IsolatorContract, CleanOnAnotherCoreIsConsistentWithACoreLocalFault) {
  ScriptedRerunner rerunner({{Rung::kSameCore, RerunOutcome::kReproduced},
                             {Rung::kOtherCore, RerunOutcome::kClean},
                             {Rung::kSingleThread, RerunOutcome::kReproduced}});
  const auto report = this->run(rerunner);
  EXPECT_EQ(rerunner.requests.size(), 3U);
  EXPECT_EQ(report.answers.other_core, Answer::kNo);
  EXPECT_EQ(report.finding, Finding::kCoreLocal);
}

TYPED_TEST_P(IsolatorContract, CleanSingleThreadedIsConcurrencyDependent) {
  ScriptedRerunner rerunner({{Rung::kSameCore, RerunOutcome::kReproduced},
                             {Rung::kOtherCore, RerunOutcome::kReproduced},
                             {Rung::kSingleThread, RerunOutcome::kClean}});
  const auto report = this->run(rerunner);
  EXPECT_EQ(report.answers.single_thread, Answer::kNo);
  EXPECT_EQ(report.finding, Finding::kConcurrencyDependent);
  EXPECT_EQ(report.confidence, verification::Confidence::kLow);
}

TYPED_TEST_P(IsolatorContract, WithNoOtherCoreTheSecondRungIsSkippedAndUnknown) {
  ScriptedRerunner rerunner({{Rung::kSameCore, RerunOutcome::kReproduced},
                             {Rung::kSingleThread, RerunOutcome::kReproduced}});
  const auto report = this->run(rerunner, verification::Placement{5, 4, {}});
  EXPECT_EQ(rerunner.requests,
            (std::vector<RerunRequest>{{Rung::kSameCore, 5, 4}, {Rung::kSingleThread, 5, 1}}));
  EXPECT_EQ(report.answers.other_core, Answer::kUnknown);
  EXPECT_EQ(report.finding, Finding::kReproducibleUnlocated);
}

TYPED_TEST_P(IsolatorContract, AnOriginalSingleThreadSkipsTheThirdRungAndKeepsItsAnswer) {
  ScriptedRerunner rerunner({{Rung::kSameCore, RerunOutcome::kReproduced},
                             {Rung::kOtherCore, RerunOutcome::kReproduced}});
  const auto report = this->run(rerunner, verification::Placement{5, 1, {9}});
  EXPECT_EQ(rerunner.requests,
            (std::vector<RerunRequest>{{Rung::kSameCore, 5, 1}, {Rung::kOtherCore, 9, 1}}));
  EXPECT_EQ(report.answers.single_thread, Answer::kYes) << "step 1 was the single-threaded run";
  EXPECT_EQ(report.finding, Finding::kNotCoreLocal);
}

TYPED_TEST_P(IsolatorContract, AnOtherCoreRerunThatCannotBeMadeLeavesTheLadderRunning) {
  ScriptedRerunner rerunner({{Rung::kSameCore, RerunOutcome::kReproduced},
                             {Rung::kOtherCore, RerunOutcome::kNotPerformed},
                             {Rung::kSingleThread, RerunOutcome::kReproduced}});
  const auto report = this->run(rerunner);
  EXPECT_EQ(rerunner.requests.size(), 3U);
  EXPECT_EQ(report.answers.other_core, Answer::kUnknown);
  EXPECT_EQ(report.finding, Finding::kReproducibleUnlocated);
}

TYPED_TEST_P(IsolatorContract, ABoundedFailureClimbsTheSameLadder) {
  ScriptedRerunner rerunner({{Rung::kSameCore, RerunOutcome::kClean}});
  const auto isolator = TypeParam::make();
  const auto report = isolator->isolate(isolation_suite::numerical_finding(),
                                        isolation_suite::kFourThreads, rerunner);
  EXPECT_EQ(report.finding, Finding::kNonReproducibleTransient);
  EXPECT_EQ(rerunner.requests.size(), 1U);
}

REGISTER_TYPED_TEST_SUITE_P(IsolatorContract, CleanOnTheSameCoreIsATransientAndStops,
                            ASameCoreRerunThatCannotBeMadeIsInconclusiveAndStops,
                            ReproducedEverywhereIsNotCoreLocalWithHighConfidence,
                            CleanOnAnotherCoreIsConsistentWithACoreLocalFault,
                            CleanSingleThreadedIsConcurrencyDependent,
                            WithNoOtherCoreTheSecondRungIsSkippedAndUnknown,
                            AnOriginalSingleThreadSkipsTheThirdRungAndKeepsItsAnswer,
                            AnOtherCoreRerunThatCannotBeMadeLeavesTheLadderRunning,
                            ABoundedFailureClimbsTheSameLadder);

}  // namespace loadforge::contracts

#endif
