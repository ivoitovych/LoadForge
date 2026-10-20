// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference isolator, run against the isolation suite, and the
// definitions src/verification/isolation.hpp carries: the ladder's table,
// confidence, and names.

#include <memory>
#include <optional>

#include "contracts/equality.hpp"
#include "contracts/verification/isolation_contract.hpp"
#include "contracts/verification/record_contract.hpp"
#include "verification/isolation.hpp"

namespace loadforge::contracts {
namespace {

using verification::Answer;
using verification::Confidence;
using verification::Finding;
using verification::IsolationReport;
using verification::RerunOutcome;
using verification::Rung;

Answer answer_of(RerunOutcome outcome) {
  switch (outcome) {
    case RerunOutcome::kReproduced:
      return Answer::kYes;
    case RerunOutcome::kClean:
      return Answer::kNo;
    default:
      return Answer::kUnknown;
  }
}

class ReferenceIsolatorImpl final : public verification::Isolator {
 public:
  IsolationReport isolate(const verification::WrongResult& finding,
                          const verification::Placement& placement,
                          verification::Rerunner& rerunner) override {
    IsolationReport report;
    const auto step = [&](Rung rung, std::uint32_t cpu, std::uint32_t threads) {
      const verification::RerunRequest request{rung, cpu, threads};
      const RerunOutcome outcome = rerunner.rerun(finding, request);
      report.steps.push_back({request, outcome});
      return answer_of(outcome);
    };
    report.answers.same_core = step(Rung::kSameCore, placement.cpu, placement.threads);
    if (report.answers.same_core == Answer::kYes) {
      if (!placement.other_cores.empty()) {
        report.answers.other_core =
            step(Rung::kOtherCore, placement.other_cores[0], placement.threads);
      }
      report.answers.single_thread = placement.threads == 1
                                         ? report.answers.same_core
                                         : step(Rung::kSingleThread, placement.cpu, 1);
    }
    report.finding = verification::derive_finding(report.answers);
    report.confidence = verification::confidence_of(report.finding);
    return report;
  }
};

struct ReferenceIsolator {
  static std::unique_ptr<verification::Isolator> make() {
    return std::make_unique<ReferenceIsolatorImpl>();
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, IsolatorContract, ::testing::Types<ReferenceIsolator>);

TEST(IsolationDefinition, OnlyWrongResultsBecomeWrongResults) {
  EXPECT_TRUE(verification::as_wrong_result(record_samples::exact()).has_value());
  EXPECT_TRUE(verification::as_wrong_result(record_samples::numerical()).has_value());
  EXPECT_FALSE(verification::as_wrong_result(record_samples::not_performed()).has_value())
      << "VerificationNotPerformed is not routed into isolation (verification.md §7)";
  EXPECT_EQ(verification::as_wrong_result(record_samples::exact())->index(), 0U);
  EXPECT_EQ(verification::as_wrong_result(record_samples::numerical())->index(), 1U);
}

TEST(IsolationDefinition, TheLadderTableRuleByRule) {
  using verification::derive_finding;
  for (const Answer other : {Answer::kYes, Answer::kNo, Answer::kUnknown}) {
    for (const Answer single : {Answer::kYes, Answer::kNo, Answer::kUnknown}) {
      EXPECT_EQ(derive_finding({Answer::kUnknown, other, single}), Finding::kInconclusive);
      EXPECT_EQ(derive_finding({Answer::kNo, other, single}), Finding::kNonReproducibleTransient);
    }
    EXPECT_EQ(derive_finding({Answer::kYes, other, Answer::kNo}), Finding::kConcurrencyDependent);
    EXPECT_EQ(derive_finding({Answer::kYes, other, Answer::kUnknown}), Finding::kInconclusive);
  }
  EXPECT_EQ(derive_finding({Answer::kYes, Answer::kNo, Answer::kYes}), Finding::kCoreLocal);
  EXPECT_EQ(derive_finding({Answer::kYes, Answer::kYes, Answer::kYes}), Finding::kNotCoreLocal);
  EXPECT_EQ(derive_finding({Answer::kYes, Answer::kUnknown, Answer::kYes}),
            Finding::kReproducibleUnlocated);
}

TEST(IsolationDefinition, ConfidenceIsHighOnlyForTheStrongestEvidence) {
  using verification::confidence_of;
  EXPECT_EQ(confidence_of(Finding::kNotCoreLocal), Confidence::kHigh);
  EXPECT_EQ(confidence_of(Finding::kCoreLocal), Confidence::kMedium);
  EXPECT_EQ(confidence_of(Finding::kReproducibleUnlocated), Confidence::kMedium);
  EXPECT_EQ(confidence_of(Finding::kNonReproducibleTransient), Confidence::kLow);
  EXPECT_EQ(confidence_of(Finding::kConcurrencyDependent), Confidence::kLow);
  EXPECT_EQ(confidence_of(Finding::kInconclusive), Confidence::kLow);
  EXPECT_EQ(confidence_of(static_cast<Finding>(99)), Confidence::kLow);
}

TEST(IsolationDefinition, EveryEnumeratorHasItsWords) {
  using verification::describe;
  EXPECT_EQ(describe(Rung::kSameCore), "same core");
  EXPECT_EQ(describe(Rung::kOtherCore), "another core");
  EXPECT_EQ(describe(Rung::kSingleThread), "single thread");
  EXPECT_EQ(describe(static_cast<Rung>(9)), "unrecognised rung");
  EXPECT_EQ(describe(RerunOutcome::kReproduced), "reproduced");
  EXPECT_EQ(describe(RerunOutcome::kClean), "clean");
  EXPECT_EQ(describe(RerunOutcome::kNotPerformed), "not performed");
  EXPECT_EQ(describe(static_cast<RerunOutcome>(9)), "unrecognised rerun outcome");
  EXPECT_EQ(describe(Answer::kYes), "yes");
  EXPECT_EQ(describe(Answer::kNo), "no");
  EXPECT_EQ(describe(Answer::kUnknown), "unknown");
  EXPECT_EQ(describe(static_cast<Answer>(9)), "unrecognised answer");
  for (const Finding finding :
       {Finding::kNonReproducibleTransient, Finding::kCoreLocal, Finding::kNotCoreLocal,
        Finding::kConcurrencyDependent, Finding::kReproducibleUnlocated, Finding::kInconclusive}) {
    EXPECT_NE(describe(finding).find(':'), std::string_view::npos)
        << "each finding says what it is consistent with";
  }
  EXPECT_NE(describe(Finding::kNotCoreLocal).find("consistent with"), std::string_view::npos);
  EXPECT_EQ(describe(static_cast<Finding>(99)), "unrecognised finding");
  EXPECT_EQ(describe(Confidence::kLow), "low");
  EXPECT_EQ(describe(Confidence::kMedium), "medium");
  EXPECT_EQ(describe(Confidence::kHigh), "high");
  EXPECT_EQ(describe(static_cast<Confidence>(9)), "unrecognised confidence");
}

TEST(IsolationEquality, EveryMemberIsCompared) {
  using verification::Answers;
  expect_memberwise_equality(Answers{Answer::kYes, Answer::kNo, Answer::kUnknown},
                             {[](Answers& a) { a.same_core = Answer::kNo; },
                              [](Answers& a) { a.other_core = Answer::kYes; },
                              [](Answers& a) { a.single_thread = Answer::kYes; }});
  using verification::RerunRequest;
  using verification::Step;
  expect_memberwise_equality(
      RerunRequest{Rung::kSameCore, 1, 2},
      {[](RerunRequest& r) { r.rung = Rung::kOtherCore; }, [](RerunRequest& r) { r.cpu = 9; },
       [](RerunRequest& r) { r.threads = 9; }});
  expect_memberwise_equality(
      Step{{Rung::kSameCore, 1, 2}, RerunOutcome::kClean},
      {[](Step& s) { s.request.cpu = 9; }, [](Step& s) { s.outcome = RerunOutcome::kReproduced; }});
  expect_memberwise_equality(
      IsolationReport{
          {}, {Answer::kYes, Answer::kNo, Answer::kYes}, Finding::kCoreLocal, Confidence::kMedium},
      {[](IsolationReport& r) { r.steps.push_back({}); },
       [](IsolationReport& r) { r.answers.same_core = Answer::kNo; },
       [](IsolationReport& r) { r.finding = Finding::kInconclusive; },
       [](IsolationReport& r) { r.confidence = Confidence::kHigh; }});
}

}  // namespace
}  // namespace loadforge::contracts
