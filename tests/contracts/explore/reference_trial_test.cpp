// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference trial runner -- it runs nothing and measures a fixed
// timeline -- against the trial suite, and the definitions the explore header
// carries.

#include <memory>
#include <string>

#include "contracts/equality.hpp"
#include "contracts/explore/trial_contract.hpp"
#include "explore/trial.hpp"

namespace loadforge::contracts {
namespace {

using explore::Candidate;
using explore::TrialError;
using explore::TrialFault;
using explore::TrialOutcome;

class FixedRunner final : public explore::TrialRunner {
 public:
  FixedRunner() {
    capabilities_.sources.push_back(
        {"thermal",
         {{telemetry::Metric{"thermal.package", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                             telemetry::Scope::kPackage, 0, std::nullopt, "hwmon"},
           std::nullopt}}});
  }

  [[nodiscard]] const telemetry::CapabilityReport& capabilities() const override {
    return capabilities_;
  }

  core::Result<TrialOutcome, TrialError> run(const Candidate& candidate) override {
    if (candidate.workloads.empty() || candidate.threads == 0 || candidate.duration_ns <= 0) {
      return TrialError{TrialFault::kCandidate, "not runnable"};
    }
    for (const std::string& workload : candidate.workloads) {
      if (workload != "word_sum") {
        return TrialError{TrialFault::kCandidate, "unknown workload " + workload};
      }
    }
    TrialOutcome outcome;
    outcome.candidate = candidate.id;
    if (candidate.settle.has_value()) {
      if (candidate.settle->metric != "thermal.package") {
        return TrialError{TrialFault::kMetricUnavailable, candidate.settle->metric};
      }
      outcome.settled = true;
      outcome.settle_ns = candidate.settle->window_ns;
    }
    const std::int64_t start = clock_ + outcome.settle_ns;
    outcome.signature.window = {start, start + candidate.duration_ns};
    clock_ = outcome.signature.window.end_ns;
    outcome.verdict = results::Verdict::kClean;
    return outcome;
  }

 private:
  telemetry::CapabilityReport capabilities_;
  std::int64_t clock_ = 1'000;
};

struct ReferenceTrialRunner {
  static constexpr const char* kPresentGauge = "thermal.package";
  static std::unique_ptr<explore::TrialRunner> make() { return std::make_unique<FixedRunner>(); }
  static Candidate candidate() {
    return Candidate{"reference", {"word_sum"}, 2, {}, 1'000'000, std::nullopt, {}};
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, TrialRunnerContract,
                               ::testing::Types<ReferenceTrialRunner>);

TEST(TrialDefinition, FaultWordsAndSettleEquality) {
  EXPECT_EQ(explore::describe(TrialFault::kCandidate), "candidate not runnable");
  EXPECT_EQ(explore::describe(TrialFault::kMetricUnavailable), "settle metric unavailable");
  EXPECT_EQ(explore::describe(TrialFault::kRun), "trial failed to run");
  EXPECT_EQ(explore::describe(static_cast<TrialFault>(9)), "unrecognised trial fault");
  expect_memberwise_equality(
      explore::Settle{"thermal.package", 0.1, 2, 3},
      {[](explore::Settle& s) { s.metric = "x.y"; },
       [](explore::Settle& s) { s.max_slope_per_second = 0.2; },
       [](explore::Settle& s) { s.window_ns = 9; }, [](explore::Settle& s) { s.max_ns = 9; }});
}

}  // namespace
}  // namespace loadforge::contracts
