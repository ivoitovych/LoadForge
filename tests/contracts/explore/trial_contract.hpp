// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_EXPLORE_TRIAL_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_EXPLORE_TRIAL_CONTRACT_HPP

// Provider-side suite for src/explore/trial.hpp (ADR 0003, 0004).
//
// TrialRunnerContract<Subject>:
//   Subject::make()        std::unique_ptr<explore::TrialRunner>.
//   Subject::candidate()   a Candidate the runner can run, short, with no
//                          settle phase and one workload.
//   Subject::kPresentGauge the name of a gauge metric present in the runner's
//                          capabilities, for a settle phase.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "explore/trial.hpp"

namespace loadforge::contracts {

template <typename Subject>
class TrialRunnerContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(TrialRunnerContract);

using explore::Candidate;
using explore::TrialFault;

TYPED_TEST_P(TrialRunnerContract, TheCapabilitiesAreProbedOnce) {
  const auto runner = TypeParam::make();
  const telemetry::CapabilityReport first = runner->capabilities();
  ASSERT_TRUE(runner->run(TypeParam::candidate()).has_value());
  EXPECT_EQ(runner->capabilities(), first);
}

TYPED_TEST_P(TrialRunnerContract, ATrialMeasuresExactlyItsLoadWindow) {
  const auto runner = TypeParam::make();
  const Candidate candidate = TypeParam::candidate();
  const auto outcome = runner->run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  EXPECT_EQ(outcome.value().candidate, candidate.id);
  const statistics::Window window = outcome.value().signature.window;
  EXPECT_EQ(window.end_ns - window.start_ns, candidate.duration_ns);
  EXPECT_FALSE(outcome.value().settled) << "no settle phase was asked for";
  EXPECT_EQ(outcome.value().settle_ns, 0);
}

TYPED_TEST_P(TrialRunnerContract, ASettlePhaseIsMeasuredAndBounded) {
  const auto runner = TypeParam::make();
  Candidate candidate = TypeParam::candidate();
  candidate.settle = explore::Settle{TypeParam::kPresentGauge, 1.0e9, 1'000'000, 50'000'000};
  const auto outcome = runner->run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  EXPECT_GE(outcome.value().settle_ns, 0);
  EXPECT_LE(outcome.value().settle_ns, candidate.settle->max_ns);
}

TYPED_TEST_P(TrialRunnerContract, ASettleMetricThatCannotBeReadIsRefused) {
  const auto runner = TypeParam::make();
  Candidate candidate = TypeParam::candidate();
  candidate.settle = explore::Settle{"thermal.no_such_sensor", 0.05, 1'000'000, 50'000'000};
  const auto outcome = runner->run(candidate);
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, TrialFault::kMetricUnavailable);
}

TYPED_TEST_P(TrialRunnerContract, AnUnrunnableCandidateIsRefusedAsSuch) {
  const auto runner = TypeParam::make();
  std::vector<Candidate> broken(5, TypeParam::candidate());
  broken[0].workloads.clear();
  broken[1].workloads = {"no_such_workload"};
  broken[2].threads = 0;
  broken[3].duration_ns = 0;
  broken[4].duration_ns = -1;
  for (std::size_t which = 0; which < broken.size(); ++which) {
    const auto outcome = runner->run(broken[which]);
    ASSERT_FALSE(outcome.has_value()) << "case " << which;
    EXPECT_EQ(outcome.error().fault, TrialFault::kCandidate) << "case " << which;
    EXPECT_FALSE(outcome.error().detail.empty());
  }
}

REGISTER_TYPED_TEST_SUITE_P(TrialRunnerContract, TheCapabilitiesAreProbedOnce,
                            ATrialMeasuresExactlyItsLoadWindow, ASettlePhaseIsMeasuredAndBounded,
                            ASettleMetricThatCannotBeReadIsRefused,
                            AnUnrunnableCandidateIsRefusedAsSuch);

}  // namespace loadforge::contracts

#endif
