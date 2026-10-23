// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore/search.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "unit/explore/model_runner.hpp"

namespace loadforge::explore {
namespace {

using explore_test::make_candidate;
using explore_test::ModelRunner;
using explore_test::Shape;
using results::Verdict;

// A lab: each candidate id has a script of values, served in order and cycling,
// counted per id, so a test says exactly what every replicate scored.
struct Lab {
  std::map<std::string, std::vector<double>> script;
  std::map<std::string, std::size_t> served;
  ModelRunner runner{[this](const Candidate& c, std::int64_t, std::size_t) {
    const std::vector<double>& values = script[c.id];
    return values[served[c.id]++ % values.size()];
  }};

  Lab() { script["ref"] = {10.0}; }
};

SearchConfig config_for(std::vector<std::string> ids) {
  SearchConfig config;
  config.objective.metric = "thermal.package";
  config.reference = make_candidate("ref");
  for (std::string& id : ids) {
    config.candidates.push_back(make_candidate(std::move(id)));
  }
  config.base_replicates = 2;
  return config;
}

const CandidateReport& find(const SearchReport& report, const std::string& id) {
  for (const CandidateReport& entry : report.candidates) {
    if (entry.id == id) {
      return entry;
    }
  }
  ADD_FAILURE() << "no candidate " << id;
  return report.candidates.front();
}

TEST(Describe, FatesAndRankingsHaveWords) {
  EXPECT_EQ(describe(Fate::kSurvivor), "survivor");
  EXPECT_EQ(describe(Fate::kEliminated), "eliminated");
  EXPECT_EQ(describe(Fate::kFindings), "findings");
  EXPECT_EQ(describe(Fate::kIncomplete), "incomplete");
  EXPECT_EQ(describe(static_cast<Fate>(200)), "unrecognised fate");
  EXPECT_EQ(describe(Ranking::kNone), "no worst case");
  EXPECT_EQ(describe(Ranking::kSole), "sole candidate");
  EXPECT_EQ(describe(Ranking::kSeparated),
            "separated from the rival with the worst corrected mean");
  EXPECT_EQ(describe(Ranking::kIndistinguishable),
            "not distinguishable from the rival with the worst corrected mean");
  EXPECT_EQ(describe(static_cast<Ranking>(200)), "unrecognised ranking");
}

// ---- configuration: everything refused before the first trial ---------------

class Configuration : public ::testing::Test {
 protected:
  Lab lab;
  void expect_refused(SearchConfig config, ExploreFault fault, const std::string& mentions) const {
    const auto search = Search::configure(std::move(config), lab.runner.capabilities());
    ASSERT_FALSE(search.has_value());
    EXPECT_EQ(search.error().fault, fault) << search.error().detail;
    EXPECT_NE(search.error().detail.find(mentions), std::string::npos) << search.error().detail;
    EXPECT_TRUE(lab.runner.ran.empty()) << "configuration must not run a trial";
  }
};

TEST_F(Configuration, AWellFormedSearchIsAccepted) {
  EXPECT_TRUE(Search::configure(config_for({"a", "b"}), lab.runner.capabilities()).has_value());
}

TEST_F(Configuration, AnObjectiveOnAnUnavailableMetricFailsHere) {
  SearchConfig config = config_for({"a"});
  config.objective.metric = "thermal.vrm";
  expect_refused(std::move(config), ExploreFault::kMetricUnavailable, "load the coretemp module");
}

TEST_F(Configuration, AnObjectiveOnAnUnknownMetricFailsHere) {
  SearchConfig config = config_for({"a"});
  config.objective.metric = "fan.cpu";
  expect_refused(std::move(config), ExploreFault::kUnknownMetric, "fan.cpu");
}

TEST_F(Configuration, AnObjectiveWhoseStatisticDoesNotFitFailsHere) {
  SearchConfig config = config_for({"a"});
  config.objective.statistic = CounterStatistic::kRate;
  expect_refused(std::move(config), ExploreFault::kStatistic, "rate");
}

TEST_F(Configuration, NoCandidatesIsRefused) {
  expect_refused(config_for({}), ExploreFault::kConfiguration, "no candidates");
}

TEST_F(Configuration, EachParameterHasItsFloor) {
  SearchConfig config = config_for({"a"});
  config.base_replicates = 0;
  expect_refused(config, ExploreFault::kConfiguration, "base_replicates");
  config = config_for({"a"});
  config.elimination_factor = 1;
  expect_refused(config, ExploreFault::kConfiguration, "elimination_factor");
  config = config_for({"a"});
  config.final_replicates = 1;
  expect_refused(config, ExploreFault::kConfiguration, "variance");
  config = config_for({"a"});
  config.interleave_every = 0;
  expect_refused(config, ExploreFault::kConfiguration, "interleave_every");
  config = config_for({"a"});
  config.reference_replicates = 0;
  expect_refused(config, ExploreFault::kConfiguration, "reference_replicates");
}

TEST_F(Configuration, IdsAreNamedUniqueAndNotTheReferences) {
  SearchConfig config = config_for({"a"});
  config.reference.id.clear();
  expect_refused(config, ExploreFault::kConfiguration, "reference has no id");
  expect_refused(config_for({"a", ""}), ExploreFault::kConfiguration, "no id");
  expect_refused(config_for({"a", "b", "a"}), ExploreFault::kConfiguration, "a is used twice");
  expect_refused(config_for({"a", "ref"}), ExploreFault::kConfiguration, "ref is used twice");
}

TEST_F(Configuration, CandidatesVerifyAtOneDutyCycleOrTheSearchIsRefused) {
  SearchConfig config = config_for({"a", "b"});
  config.candidates[1].duty.oracle_period = 8;
  expect_refused(config, ExploreFault::kDutyCycle, "candidate b");
  config = config_for({"a", "b"});
  config.reference.duty.integrity_period = 4;
  expect_refused(config, ExploreFault::kDutyCycle, "candidate a");
  config = config_for({"a", "b"});
  config.reference.duty = config.candidates[0].duty = config.candidates[1].duty = {8, 4};
  EXPECT_TRUE(Search::configure(config, lab.runner.capabilities()).has_value());
}

TEST_F(Configuration, ASettlePhaseOnAMetricThatCannotBeReadIsRefusedBeforeTheRun) {
  SearchConfig config = config_for({"a", "b"});
  config.candidates[1].settle = Settle{"thermal.vrm", 0.05, 1000, 2000};
  expect_refused(config, ExploreFault::kMetricUnavailable, "load the coretemp module");
  config = config_for({"a", "b"});
  config.candidates[0].settle = Settle{"thermal.no_such", 0.05, 1000, 2000};
  expect_refused(config, ExploreFault::kUnknownMetric, "thermal.no_such");
  config = config_for({"a"});
  config.reference.settle = Settle{"power.energy", 0.05, 1000, 2000};
  expect_refused(config, ExploreFault::kConfiguration, "not a gauge");
  config = config_for({"a"});
  config.reference.settle = Settle{"thermal.vrm", 0.05, 1000, 2000};
  expect_refused(config, ExploreFault::kMetricUnavailable, "ref");
}

// The slope is judged only once a full window has been read and the phase ends at its
// maximum, so a window longer than the maximum cannot settle. Review verdicts
// 2026-10-23, finding 3.
TEST_F(Configuration, ASettleWindowLongerThanTheSettleMaximumIsRefusedBeforeTheRun) {
  SearchConfig config = config_for({"a", "b"});
  config.candidates[1].settle = Settle{"thermal.package", 0.05, 2001, 2000};
  expect_refused(config, ExploreFault::kConfiguration, "longer than its settle maximum");
  config = config_for({"a"});
  config.reference.settle = Settle{"thermal.package", 0.05, 2001, 2000};
  expect_refused(config, ExploreFault::kConfiguration, "ref");
  config = config_for({"a"});
  config.reference.settle = config.candidates[0].settle =
      Settle{"thermal.package", 0.05, 2000, 2000};
  EXPECT_TRUE(Search::configure(config, lab.runner.capabilities()).has_value())
      << "equal is allowed";
}

TEST_F(Configuration, ASettlePhaseOnAGaugeThatIsPresentIsAccepted) {
  SearchConfig config = config_for({"a", "b"});
  config.reference.settle = config.candidates[0].settle =
      Settle{"thermal.package", 0.05, 1000, 2000};
  ASSERT_TRUE(Search::configure(config, lab.runner.capabilities()).has_value());
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  const auto report = explore(lab.runner, config);
  ASSERT_TRUE(report.has_value()) << report.error().detail;
  EXPECT_EQ(report.value().worst_case->candidate, "b");
}

// ---- running ---------------------------------------------------------------

TEST(Run, TheWorstCandidateWinsAndSaysHowSure) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {70.0};
  lab.script["c"] = {60.0};
  lab.script["d"] = {40.0};
  const auto result = explore(lab.runner, config_for({"a", "b", "c", "d"}));
  ASSERT_TRUE(result.has_value()) << result.error().detail;
  const SearchReport& report = result.value();
  ASSERT_TRUE(report.worst_case.has_value());
  EXPECT_EQ(report.worst_case->candidate, "b");
  EXPECT_EQ(report.worst_case->replicates, 4U);
  EXPECT_DOUBLE_EQ(report.worst_case->mean, 70.0);
  EXPECT_DOUBLE_EQ(report.worst_case->stddev, 0.0);
  EXPECT_DOUBLE_EQ(report.worst_case->half_width, 0.0);
  EXPECT_EQ(report.ranking, Ranking::kSeparated);
  EXPECT_EQ(report.rounds, 2U);
  // Anchor 2; pass 0 (4 trials) and a reference; pass 1 (4); round 1 with
  // b and c: passes 2 and 3 (4 trials) and a reference after the fourth.
  EXPECT_EQ(report.trials, 17U);
  EXPECT_EQ(lab.runner.ran.size(), 17U);
  EXPECT_EQ(find(report, "b").fate, Fate::kSurvivor);
  EXPECT_EQ(find(report, "b").rounds, 2U);
  EXPECT_EQ(find(report, "c").fate, Fate::kEliminated);
  EXPECT_EQ(find(report, "c").rounds, 2U);
  EXPECT_EQ(find(report, "a").fate, Fate::kEliminated);
  EXPECT_EQ(find(report, "a").rounds, 1U);
  EXPECT_EQ(find(report, "d").samples.size(), 2U);
  EXPECT_EQ(report.candidates.size(), 4U);
}

TEST(Run, TheReferenceIsRunBeforeAnyCandidateAndBetweenTrials) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  SearchConfig config = config_for({"a", "b"});
  config.interleave_every = 3;
  const auto result = explore(lab.runner, config);
  ASSERT_TRUE(result.has_value());
  // The anchor; round 0 (two passes over a and b) with a reference after its
  // third trial and a closing one after the fourth; b wins and is topped up to
  // three replicates, which a closing reference follows.
  const std::vector<std::string> expected = {"ref", "ref", "a",   "b", "a",
                                             "ref", "b",   "ref", "b", "ref"};
  EXPECT_EQ(lab.runner.ran, expected);
}

TEST(Run, TheClosingReferenceIsSkippedWhenOneJustRan) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  SearchConfig config = config_for({"a", "b"});
  config.interleave_every = 1;
  const auto result = explore(lab.runner, config);
  ASSERT_TRUE(result.has_value());
  const std::vector<std::string> expected = {"ref", "ref", "a", "ref", "b", "ref",
                                             "a",   "ref", "b", "ref", "b", "ref"};
  EXPECT_EQ(lab.runner.ran, expected);
}

TEST(Run, LowerIsWorseFindsTheLowest) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {70.0};
  lab.script["c"] = {30.0};
  SearchConfig config = config_for({"a", "b", "c"});
  config.objective.direction = Direction::kLowerIsWorse;
  const auto result = explore(lab.runner, config);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().worst_case->candidate, "c");
  EXPECT_DOUBLE_EQ(result.value().worst_case->mean, 30.0);
  EXPECT_EQ(result.value().ranking, Ranking::kSeparated);
}

TEST(Run, ACounterObjectiveRanksTheRate) {
  Lab lab;
  lab.runner.shape_of = [](const Candidate&) { return Shape::kCounter; };
  lab.script["a"] = {100.0};
  lab.script["b"] = {300.0};
  SearchConfig config = config_for({"a", "b"});
  config.objective.metric = "power.energy";
  config.objective.statistic = CounterStatistic::kRate;
  const auto result = explore(lab.runner, config);
  ASSERT_TRUE(result.has_value()) << result.error().detail;
  EXPECT_EQ(result.value().worst_case->candidate, "b");
  EXPECT_DOUBLE_EQ(result.value().worst_case->mean, 150.0);
}

TEST(Run, OneCandidateIsRunToTheFinalReplicatesAndIsSole) {
  Lab lab;
  lab.script["a"] = {50.0, 52.0, 54.0};
  const auto result = explore(lab.runner, config_for({"a"}));
  ASSERT_TRUE(result.has_value());
  const SearchReport& report = result.value();
  EXPECT_EQ(report.rounds, 0U);
  EXPECT_EQ(report.trials, 6U);  // anchor 2, three replicates, one closing reference
  ASSERT_TRUE(report.worst_case.has_value());
  EXPECT_EQ(report.worst_case->replicates, 3U);
  EXPECT_DOUBLE_EQ(report.worst_case->mean, 52.0);
  EXPECT_DOUBLE_EQ(report.worst_case->stddev, 2.0);
  // Three reference readings (the anchor's two and the closing one) estimate the
  // reference's noise on one degree of freedom, and the interval's t is looked up
  // at the smaller of that and the replicates' two.
  EXPECT_NEAR(report.worst_case->half_width,
              t_critical(reference_degrees(3)) * 2.0 / std::sqrt(3.0), 1e-9);
  EXPECT_EQ(report.references.size(), 3U);
  EXPECT_EQ(report.ranking, Ranking::kSole);
  EXPECT_EQ(find(report, "a").fate, Fate::kSurvivor);
}

TEST(Run, TheSurvivorIsToppedUpWhenHalvingEndsEarly) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  SearchConfig config = config_for({"a", "b"});
  config.base_replicates = 1;
  config.final_replicates = 5;
  const auto result = explore(lab.runner, config);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().worst_case->candidate, "b");
  EXPECT_EQ(result.value().worst_case->replicates, 5U);
  // a was eliminated on one replicate: no interval, so no separation is claimed.
  EXPECT_EQ(find(result.value(), "a").corrected.replicates, 1U);
  EXPECT_FALSE(find(result.value(), "a").corrected.half_width.has_value());
  EXPECT_EQ(result.value().ranking, Ranking::kIndistinguishable);
}

// With one replicate in the first round a candidate can leave the search on a single
// noisy reading and never get an interval. "Separated" is then a claim about the one
// rival it was compared with, and the report names that rival and the candidates it
// did not compare with an interval. Review verdicts 2026-10-23, finding 6.
TEST(Run, SeparatedNamesTheRivalAndTheCandidatesNeverComparedByAnInterval) {
  Lab lab;
  lab.script["A"] = {70.0};  // the true worst case, unlucky on its one reading
  lab.script["B"] = {90.0};
  lab.script["C"] = {88.0};
  lab.script["D"] = {60.0};
  SearchConfig config = config_for({"A", "B", "C", "D"});
  config.base_replicates = 1;
  config.final_replicates = 2;
  const auto result = explore(lab.runner, config);
  ASSERT_TRUE(result.has_value());
  const SearchReport& report = result.value();
  EXPECT_EQ(report.worst_case->candidate, "B");
  EXPECT_EQ(report.ranking, Ranking::kSeparated);
  EXPECT_EQ(report.rival, "C");
  EXPECT_EQ(report.uncompared, (std::vector<std::string>{"A", "D"}));
  EXPECT_EQ(find(report, "A").samples.size(), 1U);
}

TEST(Run, ASoleCandidateHasNoRivalAndNothingUncompared) {
  Lab lab;
  lab.script["a"] = {50.0};
  const auto result = explore(lab.runner, config_for({"a"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value().rival.empty());
  EXPECT_TRUE(result.value().uncompared.empty());
}

TEST(Run, AConstantObjectiveRanksNothing) {
  Lab lab;
  for (const char* id : {"a", "b", "c", "d"}) {
    lab.script[id] = {42.0};
  }
  const auto result = explore(lab.runner, config_for({"a", "b", "c", "d"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().ranking, Ranking::kIndistinguishable);
  EXPECT_EQ(result.value().worst_case->candidate, "a");  // the earlier on every tie
  EXPECT_DOUBLE_EQ(result.value().worst_case->mean, 42.0);
}

TEST(Run, OverlappingIntervalsAreNotSeparated) {
  Lab lab;
  lab.script["a"] = {50.0, 54.0};
  lab.script["b"] = {51.0, 55.0};
  const auto result = explore(lab.runner, config_for({"a", "b"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().worst_case->candidate, "b");
  EXPECT_EQ(result.value().ranking, Ranking::kIndistinguishable);
}

TEST(Run, DriftIsMeasuredAgainstTheAnchorAndTakenOut) {
  // Every reading, reference included, gains one degree per trial slot of
  // 1,000,000 ns: a ramp. The machine warms while the search runs.
  const auto ramp = [](double base, std::int64_t mid_ns) {
    return base + static_cast<double>(mid_ns) / 1.0e6;
  };
  std::map<std::string, double> base = {{"ref", 10.0}, {"a", 50.0}, {"b", 55.0}, {"c", 52.0}};
  ModelRunner runner(
      [&](const Candidate& c, std::int64_t mid, std::size_t) { return ramp(base.at(c.id), mid); });
  const auto result = explore(runner, config_for({"a", "b", "c"}));
  ASSERT_TRUE(result.has_value()) << result.error().detail;
  const SearchReport& report = result.value();
  EXPECT_EQ(report.worst_case->candidate, "b");
  EXPECT_GT(report.drift, 5.0) << "the ramp moved the reference";
  // A ramp is linear, so the interpolated baseline is exact: every corrected
  // replicate of a candidate is the same number.
  EXPECT_NEAR(report.worst_case->stddev, 0.0, 1e-9);
  EXPECT_NEAR(find(report, "a").corrected.mean - find(report, "c").corrected.mean, -2.0, 1e-9);
  // And the raw scores were not: the later replicates were far above the first.
  EXPECT_GT(find(report, "b").raw.stddev.value_or(0.0), 1.0);
  EXPECT_DOUBLE_EQ(report.anchor, (report.references[0].value + report.references[1].value) / 2);
  EXPECT_EQ(report.reference.replicates, report.references.size());
}

TEST(Run, ANoisyReferenceWidensTheIntervalAndSaysSo) {
  Lab steady;
  steady.script["a"] = {50.0, 51.0, 52.0};
  Lab noisy;
  noisy.script["a"] = {50.0, 51.0, 52.0};
  noisy.script["ref"] = {10.0, 14.0, 7.0, 13.0, 9.0};
  const SearchConfig config = config_for({"a"});
  const auto quiet = explore(steady.runner, config);
  const auto loud = explore(noisy.runner, config);
  ASSERT_TRUE(quiet.has_value());
  ASSERT_TRUE(loud.has_value());
  EXPECT_DOUBLE_EQ(quiet.value().worst_case->correction_stddev, 0.0);
  EXPECT_GT(loud.value().worst_case->correction_stddev, 0.5);
  EXPECT_GT(loud.value().worst_case->half_width, quiet.value().worst_case->half_width);
  EXPECT_NE(format(*loud.value().worst_case).find("correction sd"), std::string::npos);
}

// ---- trials that go wrong --------------------------------------------------

TEST(Run, ATrialThatFindsSomethingIsAFindingNotAScore) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {999.0};  // the highest score, and the most important thing it did
  lab.script["c"] = {60.0};
  lab.runner.verdict = [](const Candidate& c, std::size_t) {
    return c.id == "b" ? Verdict::kFindings : Verdict::kClean;
  };
  const auto result = explore(lab.runner, config_for({"a", "b", "c"}));
  ASSERT_TRUE(result.has_value());
  const SearchReport& report = result.value();
  EXPECT_EQ(find(report, "b").fate, Fate::kFindings);
  EXPECT_TRUE(find(report, "b").samples.empty());
  EXPECT_EQ(report.worst_case->candidate, "c");
  EXPECT_DOUBLE_EQ(report.worst_case->mean, 60.0);
}

TEST(Run, AnIncompleteTrialRemovesItsCandidateToo) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {999.0};
  lab.runner.verdict = [](const Candidate& c, std::size_t) {
    return c.id == "b" ? Verdict::kIncomplete : Verdict::kClean;
  };
  const auto result = explore(lab.runner, config_for({"a", "b"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(find(result.value(), "b").fate, Fate::kIncomplete);
  EXPECT_EQ(result.value().worst_case->candidate, "a");
  EXPECT_EQ(result.value().ranking, Ranking::kSole);
}

// A trial the hardware reported a corrected error in computed the right thing, so
// its number is a measurement and is scored; it is flagged, per trial, so that
// the report and the exit code can say it (stress exits 5 for it too). Dropping the
// trial would hide the candidate that stresses the hardware most, which is the
// one explore is looking for. Review verdicts 2026-10-23, finding 1.
TEST(Run, CorrectedErrorsAreScoredAndFlaggedPerTrial) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  std::size_t b_runs = 0;
  lab.runner.verdict = [&](const Candidate& c, std::size_t) {
    return c.id == "b" && ++b_runs == 2 ? Verdict::kCorrectedErrors : Verdict::kClean;
  };
  const auto result = explore(lab.runner, config_for({"a", "b"}));
  ASSERT_TRUE(result.has_value());
  const SearchReport& report = result.value();
  EXPECT_EQ(report.worst_case->candidate, "b");
  ASSERT_EQ(find(report, "b").notes.size(), find(report, "b").samples.size());
  EXPECT_FALSE(find(report, "b").notes[0].corrected_errors);
  EXPECT_TRUE(find(report, "b").notes[1].corrected_errors);
  for (const TrialNote& note : find(report, "a").notes) {
    EXPECT_FALSE(note.corrected_errors);
  }
  EXPECT_EQ(report.corrected_error_trials, 1U);
  EXPECT_EQ(report.reference_notes.size(), report.references.size());
}

TEST(Run, CorrectedErrorsInTheEveryTrialAreAllCounted) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  lab.runner.verdict = [](const Candidate&, std::size_t) { return Verdict::kCorrectedErrors; };
  const auto result = explore(lab.runner, config_for({"a", "b"}));
  ASSERT_TRUE(result.has_value());
  const SearchReport& report = result.value();
  std::size_t flagged = 0;
  for (const CandidateReport& entry : report.candidates) {
    for (const TrialNote& note : entry.notes) {
      flagged += static_cast<std::size_t>(note.corrected_errors);
    }
  }
  for (const TrialNote& note : report.reference_notes) {
    EXPECT_TRUE(note.corrected_errors);
    ++flagged;
  }
  EXPECT_EQ(report.corrected_error_trials, flagged);
  EXPECT_EQ(report.corrected_error_trials, report.trials);
}

// A settle phase that ends at its maximum is documented as no error (a trial still
// runs), but the controlled starting state the search promises is then missing for
// that trial, and a report that said nothing would pass it off as settled.
// Review verdicts 2026-10-23, finding 2.
void ask_to_settle(SearchConfig& config) {
  const Settle phase{"thermal.package", 0.05, 30'000'000'000, 300'000'000'000};
  config.reference.settle = phase;
  for (Candidate& candidate : config.candidates) {
    candidate.settle = phase;
  }
}

TEST(Run, ATrialThatDidNotSettleIsMarkedAndCounted) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  SearchConfig config = config_for({"a", "b"});
  ask_to_settle(config);
  std::size_t b_runs = 0;
  lab.runner.settles = [&](const Candidate& c, std::size_t) {
    return !(c.id == "b" && ++b_runs == 2);
  };
  const auto result = explore(lab.runner, config);
  ASSERT_TRUE(result.has_value());
  const SearchReport& report = result.value();
  EXPECT_EQ(report.settle_trials, report.trials);
  EXPECT_EQ(report.unsettled_trials, 1U);
  EXPECT_FALSE(find(report, "b").notes[0].unsettled);
  EXPECT_TRUE(find(report, "b").notes[1].unsettled);
  for (const TrialNote& note : find(report, "a").notes) {
    EXPECT_FALSE(note.unsettled);
  }
  for (const TrialNote& note : report.reference_notes) {
    EXPECT_FALSE(note.unsettled);
  }
}

TEST(Run, EveryTrialUnsettledIsCountedIncludingTheReferenceAndUnscoredTrials) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  SearchConfig config = config_for({"a", "b"});
  ask_to_settle(config);
  lab.runner.settles = [](const Candidate&, std::size_t) { return false; };
  lab.runner.verdict = [](const Candidate& c, std::size_t) {
    return c.id == "a" ? Verdict::kFindings : Verdict::kClean;
  };
  const auto result = explore(lab.runner, config);
  ASSERT_TRUE(result.has_value());
  const SearchReport& report = result.value();
  EXPECT_EQ(report.unsettled_trials, report.trials);
  for (const TrialNote& note : report.reference_notes) {
    EXPECT_TRUE(note.unsettled);
  }
  EXPECT_TRUE(find(report, "b").notes.back().unsettled);
}

TEST(Run, ASearchWithNoSettlePhaseCountsNoSettleTrials) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  lab.runner.settles = [](const Candidate&, std::size_t) { return false; };
  const auto result = explore(lab.runner, config_for({"a", "b"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().settle_trials, 0U);
  EXPECT_EQ(result.value().unsettled_trials, 0U);
}

TEST(Run, WhenEveryCandidateFindsSomethingThereIsNoWorstCase) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  lab.runner.verdict = [](const Candidate& c, std::size_t) {
    if (c.id == "ref") {
      return Verdict::kClean;
    }
    return c.id == "a" ? Verdict::kFindings : Verdict::kIncomplete;
  };
  const auto result = explore(lab.runner, config_for({"a", "b"}));
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result.value().worst_case.has_value());
  EXPECT_EQ(result.value().ranking, Ranking::kNone);
  EXPECT_EQ(find(result.value(), "a").fate, Fate::kFindings);
  EXPECT_EQ(find(result.value(), "b").fate, Fate::kIncomplete);
}

TEST(Run, AWinnerThatFindsSomethingInTheTopUpLeavesNoClaim) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  SearchConfig config = config_for({"a", "b"});
  config.base_replicates = 1;
  config.final_replicates = 4;
  std::size_t b_runs = 0;
  lab.runner.verdict = [&](const Candidate& c, std::size_t) {
    return c.id == "b" && ++b_runs == 3 ? Verdict::kFindings : Verdict::kClean;
  };
  const auto result = explore(lab.runner, config);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(find(result.value(), "b").fate, Fate::kFindings);
  EXPECT_FALSE(result.value().worst_case.has_value());
  EXPECT_EQ(result.value().ranking, Ranking::kNone);
}

TEST(Run, ARunnerFaultStopsTheSearchNamingTheCandidateAndTheFault) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  lab.runner.fault = [](const Candidate& c, std::size_t) -> std::optional<explore::TrialError> {
    if (c.id == "b") {
      return explore::TrialError{TrialFault::kRun, "worker 2 did not start"};
    }
    return std::nullopt;
  };
  const auto result = explore(lab.runner, config_for({"a", "b"}));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kTrial);
  ASSERT_TRUE(result.error().trial.has_value());
  EXPECT_EQ(*result.error().trial, TrialFault::kRun);
  EXPECT_EQ(result.error().detail, "b: worker 2 did not start");
}

TEST(Run, ARunnerFaultOnTheReferenceStopsItBeforeAnyCandidate) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.runner.fault = [](const Candidate& c, std::size_t) -> std::optional<explore::TrialError> {
    if (c.id == "ref") {
      return explore::TrialError{TrialFault::kRun, "controller stopped"};
    }
    return std::nullopt;
  };
  const auto result = explore(lab.runner, config_for({"a"}));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kTrial);
  EXPECT_EQ(lab.runner.ran, std::vector<std::string>{"ref"});
}

// A reference that cannot be run at one chosen moment: after `calls` runs.
std::optional<explore::TrialError> reference_down_after(const Candidate& c, std::size_t call,
                                                        std::size_t calls) {
  if (c.id == "ref" && call >= calls) {
    return explore::TrialError{TrialFault::kRun, "controller stopped"};
  }
  return std::nullopt;
}

TEST(Run, ARunnerFaultWhileClosingARoundStopsTheSearch) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  SearchConfig config = config_for({"a", "b"});
  config.interleave_every = 100;
  config.base_replicates = 1;
  // Calls: ref, ref, a, b, then the closing reference.
  lab.runner.fault = [](const Candidate& c, std::size_t call) {
    return reference_down_after(c, call, 4);
  };
  const auto result = explore(lab.runner, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kTrial);
  EXPECT_EQ(lab.runner.ran.size(), 5U);
}

TEST(Run, ARunnerFaultInTheSurvivorsTopUpStopsTheSearch) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  SearchConfig config = config_for({"a", "b"});
  config.base_replicates = 1;
  config.interleave_every = 100;
  // Calls: ref, ref, a, b, closing ref, then b's top-up (call 5).
  lab.runner.fault = [](const Candidate& c,
                        std::size_t call) -> std::optional<explore::TrialError> {
    if (call == 5) {
      return explore::TrialError{TrialFault::kRun, "worker lost: " + c.id};
    }
    return std::nullopt;
  };
  const auto result = explore(lab.runner, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().detail, "b: worker lost: b");
}

TEST(Run, ARunnerFaultOnTheFinalClosingReferenceStopsTheSearch) {
  Lab lab;
  lab.script["a"] = {50.0};
  SearchConfig config = config_for({"a"});
  config.interleave_every = 100;
  // Calls: ref, ref, a, a, a, then the closing reference.
  lab.runner.fault = [](const Candidate& c, std::size_t call) {
    return reference_down_after(c, call, 5);
  };
  const auto result = explore(lab.runner, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kTrial);
}

TEST(Run, AReferenceThatFindsSomethingOrDoesNotFinishEndsTheSearch) {
  for (const Verdict verdict : {Verdict::kFindings, Verdict::kIncomplete}) {
    Lab lab;
    lab.script["a"] = {50.0};
    lab.runner.verdict = [verdict](const Candidate& c, std::size_t) {
      return c.id == "ref" ? verdict : Verdict::kClean;
    };
    const auto result = explore(lab.runner, config_for({"a"}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().fault, ExploreFault::kReference);
    EXPECT_NE(result.error().detail.find(std::string(results::describe(verdict))),
              std::string::npos);
  }
}

TEST(Run, AReferenceReadingLostPartWayStillEndsTheSearch) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  SearchConfig config = config_for({"a", "b"});
  config.interleave_every = 1;
  lab.runner.verdict = [](const Candidate& c, std::size_t call) {
    return c.id == "ref" && call > 3 ? Verdict::kFindings : Verdict::kClean;
  };
  const auto result = explore(lab.runner, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kReference);
}

TEST(Run, ACandidateWithNoReadingOfTheObjectiveEndsTheSearchRatherThanScoreZero) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.script["b"] = {60.0};
  lab.runner.shape_of = [](const Candidate& c) {
    return c.id == "b" ? Shape::kUnavailable : Shape::kGauge;
  };
  const auto result = explore(lab.runner, config_for({"a", "b"}));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kNoReading);
  EXPECT_NE(result.error().detail.find("load the coretemp module"), std::string::npos);
}

TEST(Run, AReferenceWithNoReadingEndsTheSearchToo) {
  Lab lab;
  lab.script["a"] = {50.0};
  lab.runner.shape_of = [](const Candidate& c) {
    return c.id == "ref" ? Shape::kMissing : Shape::kGauge;
  };
  const auto result = explore(lab.runner, config_for({"a"}));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kNoReading);
}

TEST(Explore, ConfigurationFailureRunsNothing) {
  Lab lab;
  SearchConfig config = config_for({"a"});
  config.objective.metric = "thermal.vrm";
  const auto result = explore(lab.runner, config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().fault, ExploreFault::kMetricUnavailable);
  EXPECT_TRUE(lab.runner.ran.empty());
}

TEST(Run, TheSameHistoryGivesTheSameReportBitForBit) {
  const auto go = [] {
    Lab lab;
    lab.script["a"] = {50.0, 51.5, 49.25};
    lab.script["b"] = {53.0, 52.0, 54.5};
    lab.script["c"] = {47.0, 48.0};
    return explore(lab.runner, config_for({"a", "b", "c"}));
  };
  const auto first = go();
  const auto second = go();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first.value().trials, second.value().trials);
  EXPECT_TRUE(first.value().worst_case->mean == second.value().worst_case->mean);
  EXPECT_TRUE(first.value().worst_case->half_width == second.value().worst_case->half_width);
  EXPECT_TRUE(first.value().candidates[1].samples == second.value().candidates[1].samples);
}

}  // namespace
}  // namespace loadforge::explore
