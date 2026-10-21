// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ladder: IsolatorContract run against LadderIsolator, and what this
// reading of the contract adds to the suite -- the answer of a one-thread
// original that stopped at step 1, an outcome outside the enumeration, a
// placement the contract does not admit, and that the finding a re-run is
// asked about is the finding the ladder was given.

#include "verification/isolation/ladder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <variant>
#include <vector>

#include "contracts/verification/isolation_contract.hpp"
#include "verification/isolation.hpp"
#include "verification/record.hpp"

namespace loadforge::verification::isolation {
namespace {

struct Ladder {
  static std::unique_ptr<Isolator> make() { return std::make_unique<LadderIsolator>(); }
};

}  // namespace
}  // namespace loadforge::verification::isolation

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(Ladder, IsolatorContract,
                               ::testing::Types<verification::isolation::Ladder>);
}  // namespace loadforge::contracts

namespace loadforge::verification::isolation {
namespace {

/// This module's own re-runner: answers each rung from a script and keeps the
/// findings it was asked about as well as the requests.
class Script final : public Rerunner {
 public:
  explicit Script(std::map<Rung, RerunOutcome> outcomes) : outcomes_(std::move(outcomes)) {}
  RerunOutcome rerun(const WrongResult& finding, const RerunRequest& request) override {
    findings.push_back(finding);
    requests.push_back(request);
    const auto found = outcomes_.find(request.rung);
    return found == outcomes_.end() ? RerunOutcome::kNotPerformed : found->second;
  }
  std::vector<WrongResult> findings;
  std::vector<RerunRequest> requests;

 private:
  std::map<Rung, RerunOutcome> outcomes_;
};

WrongResult exact() {
  ExactBitCorruption record;
  record.provenance.logical_cpu = 3;
  record.provenance.iteration = 12;
  record.provenance.seed = 99;
  record.check = "round trip";
  return record;
}

TEST(AnswerOf, EachOutcomeAndNothingElseAnswersYesOrNo) {
  EXPECT_EQ(answer_of(RerunOutcome::kReproduced), Answer::kYes);
  EXPECT_EQ(answer_of(RerunOutcome::kClean), Answer::kNo);
  EXPECT_EQ(answer_of(RerunOutcome::kNotPerformed), Answer::kUnknown);
  EXPECT_EQ(answer_of(static_cast<RerunOutcome>(7)), Answer::kUnknown)
      << "an outcome outside the enumeration is not a clean re-run";
}

TEST(Ladder, AOneThreadOriginalThatComesOutCleanAnswersNoSingleThreadedToo) {
  Script script({{Rung::kSameCore, RerunOutcome::kClean}});
  const auto report = LadderIsolator().isolate(exact(), Placement{3, 1, {7}}, script);
  EXPECT_EQ(script.requests, (std::vector<RerunRequest>{{Rung::kSameCore, 3, 1}}));
  EXPECT_EQ(report.answers.same_core, Answer::kNo);
  EXPECT_EQ(report.answers.single_thread, Answer::kNo) << "step 1 was the one-thread re-run";
  EXPECT_EQ(report.answers.other_core, Answer::kUnknown) << "not run";
  EXPECT_EQ(report.finding, Finding::kNonReproducibleTransient);
}

TEST(Ladder, AOneThreadOriginalThatCannotBeRerunIsUnknownSingleThreadedToo) {
  Script script({});
  const auto report = LadderIsolator().isolate(exact(), Placement{3, 1, {7}}, script);
  EXPECT_EQ(script.requests.size(), 1U);
  EXPECT_EQ(report.answers.single_thread, Answer::kUnknown);
  EXPECT_EQ(report.finding, Finding::kInconclusive);
}

TEST(Ladder, AManyThreadOriginalThatComesOutCleanLeavesTheLaterRungsUnknown) {
  Script script({{Rung::kSameCore, RerunOutcome::kClean}});
  const auto report = LadderIsolator().isolate(exact(), Placement{3, 2, {7}}, script);
  EXPECT_EQ(report.steps, (std::vector<Step>{{{Rung::kSameCore, 3, 2}, RerunOutcome::kClean}}));
  EXPECT_EQ(report.answers, (Answers{Answer::kNo, Answer::kUnknown, Answer::kUnknown}));
}

TEST(Ladder, AnOutcomeOutsideTheEnumerationStopsTheLadderAsInconclusive) {
  Script script({{Rung::kSameCore, static_cast<RerunOutcome>(7)}});
  const auto report = LadderIsolator().isolate(exact(), Placement{3, 4, {7}}, script);
  EXPECT_EQ(script.requests.size(), 1U);
  EXPECT_EQ(report.finding, Finding::kInconclusive)
      << "never a transient: nothing said the re-run was clean";
  ASSERT_EQ(report.steps.size(), 1U);
  EXPECT_EQ(report.steps[0].outcome, static_cast<RerunOutcome>(7)) << "recorded as given";
}

TEST(Ladder, OnlyTheFirstOtherCoreIsTried) {
  Script script({{Rung::kSameCore, RerunOutcome::kReproduced},
                 {Rung::kOtherCore, RerunOutcome::kNotPerformed},
                 {Rung::kSingleThread, RerunOutcome::kReproduced}});
  const auto report = LadderIsolator().isolate(exact(), Placement{3, 2, {7, 8, 9}}, script);
  EXPECT_EQ(script.requests,
            (std::vector<RerunRequest>{
                {Rung::kSameCore, 3, 2}, {Rung::kOtherCore, 7, 2}, {Rung::kSingleThread, 3, 1}}));
  EXPECT_EQ(report.finding, Finding::kReproducibleUnlocated);
}

TEST(Ladder, ANoThreadPlacementIsPassedToTheRerunnerAsGiven) {
  // The contract says a placement runs at least one thread. One that does not
  // is not corrected here: the re-runner is the one that can refuse it, and a
  // refusal is an answer the ladder already handles.
  Script script({});
  const auto report = LadderIsolator().isolate(exact(), Placement{3, 0, {}}, script);
  EXPECT_EQ(script.requests, (std::vector<RerunRequest>{{Rung::kSameCore, 3, 0}}));
  EXPECT_EQ(report.finding, Finding::kInconclusive);
}

TEST(Ladder, EveryRerunIsAskedAboutTheFindingTheLadderWasGiven) {
  Script script({{Rung::kSameCore, RerunOutcome::kReproduced},
                 {Rung::kOtherCore, RerunOutcome::kReproduced},
                 {Rung::kSingleThread, RerunOutcome::kReproduced}});
  const WrongResult finding = exact();
  const auto report = LadderIsolator().isolate(finding, Placement{3, 2, {7}}, script);
  ASSERT_EQ(script.findings.size(), 3U);
  for (const WrongResult& asked : script.findings) {
    ASSERT_TRUE(std::holds_alternative<ExactBitCorruption>(asked));
    EXPECT_EQ(std::get<ExactBitCorruption>(asked).provenance.iteration, 12U);
    EXPECT_EQ(std::get<ExactBitCorruption>(asked).provenance.seed, 99U);
    EXPECT_EQ(std::get<ExactBitCorruption>(asked).check, "round trip");
  }
  EXPECT_EQ(report.finding, Finding::kNotCoreLocal);
}

TEST(Ladder, OneIsolatorServesCallAfterCall) {
  LadderIsolator isolator;
  Script clean({{Rung::kSameCore, RerunOutcome::kClean}});
  Script everywhere({{Rung::kSameCore, RerunOutcome::kReproduced},
                     {Rung::kOtherCore, RerunOutcome::kReproduced},
                     {Rung::kSingleThread, RerunOutcome::kReproduced}});
  EXPECT_EQ(isolator.isolate(exact(), Placement{3, 2, {7}}, clean).finding,
            Finding::kNonReproducibleTransient);
  const auto second = isolator.isolate(exact(), Placement{3, 2, {7}}, everywhere);
  EXPECT_EQ(second.finding, Finding::kNotCoreLocal);
  EXPECT_EQ(second.steps.size(), 3U) << "nothing carried over from the first call";
}

}  // namespace
}  // namespace loadforge::verification::isolation
