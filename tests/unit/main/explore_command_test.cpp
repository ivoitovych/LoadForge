// SPDX-License-Identifier: GPL-3.0-or-later
//
// `loadforge explore`: the argument grammar (every option refused by name),
// the search the arguments describe, the report in words, and the exit codes.

#include "main/explore_command.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "explore/search.hpp"
#include "main/cli.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/controller/machine_tree.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::cli {
namespace {

using explore::CounterStatistic;
using explore::Direction;
using explore::Fate;
using explore::GaugeStatistic;

core::Result<ExploreArgs, std::string> parse(std::vector<std::string_view> args) {
  return parse_explore_args(args);
}

std::vector<std::string_view> with_required(std::vector<std::string_view> more) {
  std::vector<std::string_view> args{"--objective", "thermal.package:max:higher", "--candidate",
                                     "wide:sort:4"};
  args.insert(args.end(), more.begin(), more.end());
  return args;
}

std::string refusal_of(std::vector<std::string_view> args) {
  const auto parsed = parse(std::move(args));
  EXPECT_FALSE(parsed.has_value());
  return parsed.has_value() ? std::string{} : parsed.error();
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

TEST(ExploreArgs, TheShortestSearchTakesTheDefaultsOfTheSearch) {
  const auto parsed = parse(with_required({}));
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  const ExploreArgs& args = parsed.value();
  EXPECT_EQ(args.objective.metric, "thermal.package");
  EXPECT_EQ(args.objective.scope_index, 0U);
  EXPECT_EQ(args.objective.statistic,
            (std::variant<GaugeStatistic, CounterStatistic>{GaugeStatistic::kMax}));
  EXPECT_EQ(args.objective.direction, Direction::kHigherIsWorse);
  ASSERT_EQ(args.candidates.size(), 1U);
  EXPECT_EQ(args.candidates[0].id, "wide");
  EXPECT_EQ(args.candidates[0].workloads, std::vector<std::string>{"sort"});
  EXPECT_EQ(args.candidates[0].threads, 4U);
  EXPECT_TRUE(args.candidates[0].cpus.empty());
  EXPECT_FALSE(args.reference.has_value());
  EXPECT_FALSE(args.settle.has_value());
  EXPECT_EQ(args.duration_ns, 10'000'000'000);
  const explore::SearchConfig defaults;
  EXPECT_EQ(args.base_replicates, defaults.base_replicates);
  EXPECT_EQ(args.elimination_factor, defaults.elimination_factor);
  EXPECT_EQ(args.final_replicates, defaults.final_replicates);
  EXPECT_EQ(args.interleave_every, defaults.interleave_every);
  EXPECT_EQ(args.reference_replicates, defaults.reference_replicates);
  EXPECT_FALSE(args.verbose);
}

TEST(ExploreArgs, EveryOptionIsTakenInAnyOrder) {
  const auto parsed = parse({"--verbose",
                             "--max-temperature",
                             "95",
                             "--lock",
                             "require",
                             "--isa",
                             "scalar",
                             "--footprint",
                             "2MiB",
                             "--seed",
                             "7",
                             "--sample-interval",
                             "250ms",
                             "--reference-replicates",
                             "3",
                             "--interleave",
                             "2",
                             "--elimination",
                             "3",
                             "--final-replicates",
                             "5",
                             "--replicates",
                             "2",
                             "--settle-max",
                             "60s",
                             "--settle-window",
                             "10s",
                             "--settle-slope",
                             "0.5",
                             "--settle",
                             "thermal.package",
                             "--duration",
                             "90s",
                             "--reference",
                             "idle:sort+fft:2:0-1",
                             "--candidate",
                             "a:sort:1",
                             "--candidate",
                             "b:fft+sort:2:0,2",
                             "--objective",
                             "frequency.cpu@3:min:lower"});
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  const ExploreArgs& args = parsed.value();
  EXPECT_TRUE(args.verbose);
  EXPECT_EQ(args.max_temperature_c, 95);
  EXPECT_EQ(args.lock, verification::LockPolicy::kRequire);
  EXPECT_EQ(args.isa, simd::IsaPath::kScalar);
  EXPECT_EQ(args.footprint_bytes, std::uint64_t{2} << 20U);
  EXPECT_EQ(args.seed, 7U);
  EXPECT_EQ(args.sample_interval_ns, 250'000'000);
  EXPECT_EQ(args.reference_replicates, 3U);
  EXPECT_EQ(args.interleave_every, 2U);
  EXPECT_EQ(args.elimination_factor, 3U);
  EXPECT_EQ(args.final_replicates, 5U);
  EXPECT_EQ(args.base_replicates, 2U);
  ASSERT_TRUE(args.settle.has_value());
  EXPECT_EQ(*args.settle,
            (explore::Settle{"thermal.package", 0.5, 10'000'000'000, 60'000'000'000}));
  EXPECT_EQ(args.duration_ns, 90'000'000'000);
  ASSERT_TRUE(args.reference.has_value());
  EXPECT_EQ(args.reference->id, "idle");
  EXPECT_EQ(args.reference->workloads, (std::vector<std::string>{"sort", "fft"}));
  EXPECT_EQ(args.reference->cpus, (std::vector<std::uint32_t>{0, 1}));
  ASSERT_EQ(args.candidates.size(), 2U);
  EXPECT_EQ(args.candidates[1].cpus, (std::vector<std::uint32_t>{0, 2}));
  EXPECT_EQ(args.objective.metric, "frequency.cpu");
  EXPECT_EQ(args.objective.scope_index, 3U);
  EXPECT_EQ(args.objective.direction, Direction::kLowerIsWorse);
}

TEST(ExploreArgs, ACounterStatisticAndEachSettleDefaultAreWhatTheyAreCalled) {
  const auto parsed = parse({"--objective", "power.package:rate:higher", "--candidate", "a:sort:1",
                             "--settle", "thermal.package"});
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value().objective.statistic,
            (std::variant<GaugeStatistic, CounterStatistic>{CounterStatistic::kRate}));
  EXPECT_EQ(*parsed.value().settle, explore::Settle{"thermal.package"});
  for (const auto& [name, statistic] :
       {std::pair<std::string_view, std::variant<GaugeStatistic, CounterStatistic>>{
            "mean", GaugeStatistic::kMean},
        {"min", GaugeStatistic::kMin},
        {"last", GaugeStatistic::kLast},
        {"stddev", GaugeStatistic::kStddev},
        {"slope", GaugeStatistic::kSlope},
        {"delta", CounterStatistic::kDelta}}) {
    const std::string spec = "m.x:" + std::string{name} + ":higher";
    const auto each = parse({"--objective", spec, "--candidate", "a:sort:1"});
    ASSERT_TRUE(each.has_value()) << each.error();
    EXPECT_EQ(each.value().objective.statistic, statistic) << name;
  }
}

TEST(ExploreArgs, TheObjectiveAndACandidateAreRequired) {
  EXPECT_TRUE(contains(refusal_of({"--candidate", "a:sort:1"}), "--objective is required"));
  EXPECT_TRUE(contains(refusal_of({"--objective", "m.x:max:higher"}), "at least one --candidate"));
  EXPECT_TRUE(contains(refusal_of({}), "--objective is required"));
}

TEST(ExploreArgs, AnOptionIsRefusedByNameWhenUnknownTwiceOrWithoutAValue) {
  EXPECT_EQ(refusal_of(with_required({"--workers", "3"})), "--workers: not an option of explore");
  EXPECT_EQ(refusal_of(with_required({"--seed", "1", "--seed", "2"})), "--seed: given twice");
  EXPECT_EQ(refusal_of(with_required({"--verbose", "--verbose"})), "--verbose: given twice");
  EXPECT_EQ(refusal_of(with_required({"--duration"})), "--duration: missing its value");
  EXPECT_EQ(refusal_of({"--objective"}), "--objective: missing its value");
}

TEST(ExploreArgs, AnObjectiveIsRefusedByWhatIsWrongWithIt) {
  const auto bad = [](std::string_view spec) {
    return refusal_of({"--objective", spec, "--candidate", "a:sort:1"});
  };
  EXPECT_TRUE(contains(bad("thermal.package:max"), "expected METRIC[@INDEX]:STATISTIC"));
  EXPECT_TRUE(contains(bad("a:b:c:d"), "expected METRIC[@INDEX]:STATISTIC"));
  EXPECT_TRUE(contains(bad(":max:higher"), "NAME or NAME@INDEX"));
  EXPECT_TRUE(contains(bad("a@1@2:max:higher"), "NAME or NAME@INDEX"));
  EXPECT_TRUE(contains(bad("a@x:max:higher"), "expected a whole number"));
  EXPECT_TRUE(contains(bad("a@4294967296:max:higher"), "scope index is too large"));
  EXPECT_TRUE(contains(bad("a:median:higher"), "'median' is none of mean"));
  EXPECT_TRUE(contains(bad("a:max:sideways"), "'sideways' is neither higher nor lower"));
}

TEST(ExploreArgs, ACandidateIsRefusedByWhatIsWrongWithIt) {
  const auto bad = [](std::string_view spec) {
    return refusal_of({"--objective", "a.b:max:higher", "--candidate", spec});
  };
  EXPECT_TRUE(contains(bad("a:sort"), "expected ID:WORKLOAD"));
  EXPECT_TRUE(contains(bad("a:sort:1:0:9"), "expected ID:WORKLOAD"));
  EXPECT_TRUE(contains(bad(":sort:1"), "needs an id"));
  EXPECT_TRUE(contains(bad("a::1"), "an empty workload name"));
  EXPECT_TRUE(contains(bad("a:sort+:1"), "an empty workload name"));
  EXPECT_TRUE(contains(bad("a:sort:0"), "must be at least 1"));
  EXPECT_TRUE(contains(bad("a:sort:many"), "expected a whole number"));
  EXPECT_TRUE(contains(bad("a:sort:4294967296"), "too many threads"));
  EXPECT_TRUE(contains(bad("a:sort:1:3-1"), "is not a list of CPUs"));
  EXPECT_TRUE(contains(bad("a:sort:1:"), "is not a list of CPUs"));
  EXPECT_TRUE(contains(refusal_of(with_required({"--reference", "x:sort"})), "--reference:"));
}

TEST(ExploreArgs, ALengthOfTimeIsRefusedWhenItIsNotOneATrialCanHave) {
  for (const std::string_view key :
       {"--duration", "--settle-window", "--settle-max", "--sample-interval"}) {
    EXPECT_TRUE(contains(refusal_of(with_required({key, "soon"})), std::string{key} + ":"));
    EXPECT_TRUE(contains(refusal_of(with_required({key, "0s"})), "above zero"));
    EXPECT_TRUE(contains(refusal_of(with_required({key, "721h"})), "at most 30 days"));
  }
}

// A rate needs a span, a spread and a slope need two readings, a delta a pair; with the
// duration at the sample interval the first trial ran in full and then ended "no
// reading for the objective", exit 6, for what is a mistake in the options. Review
// verdicts 2026-10-23, finding 11.
TEST(ExploreArgs, ADurationThatCannotHoldTwoReadingsIsRefusedForTheStatisticsThatNeedTwo) {
  for (const std::string_view statistic : {"rate", "delta", "stddev", "slope"}) {
    const std::string objective = "thermal.package:" + std::string{statistic} + ":higher";
    const std::vector<std::string_view> short_load{"--objective",       objective,    "--candidate",
                                                   "a:sort:1",          "--duration", "1s",
                                                   "--sample-interval", "1s"};
    const std::string refusal = refusal_of(short_load);
    EXPECT_TRUE(contains(refusal, "--duration: must be at least twice --sample-interval (1000 ms)"))
        << refusal;
    EXPECT_TRUE(contains(refusal, "needs two readings in the window, and the duration is 1000 ms"));
    std::vector<std::string_view> twice = short_load;
    twice[5] = "1999ms";
    EXPECT_FALSE(parse(twice).has_value()) << statistic << ": just under two intervals";
    twice[5] = "2s";
    EXPECT_TRUE(parse(twice).has_value()) << statistic << ": two intervals hold two readings";
  }
  // The default duration is 10 s: a sample interval above 5 s is too long for it.
  EXPECT_TRUE(contains(refusal_of({"--objective", "thermal.package:rate:higher", "--candidate",
                                   "a:sort:1", "--sample-interval", "6s"}),
                       "--duration: must be at least twice --sample-interval (6000 ms)"));
}

TEST(ExploreArgs, AStatisticOfOneReadingNeedsNoMoreThanOne) {
  for (const std::string_view statistic : {"mean", "max", "min", "last"}) {
    const std::string objective = "thermal.package:" + std::string{statistic} + ":higher";
    EXPECT_TRUE(parse({"--objective", objective, "--candidate", "a:sort:1", "--duration", "1s",
                       "--sample-interval", "1s"})
                    .has_value())
        << statistic;
  }
}

TEST(ExploreArgs, TheSettleSlopeIsANumberOfZeroOrMore) {
  for (const std::string_view bad : {"", "fast", "-0.1", "nan", "inf", "0.5s"}) {
    EXPECT_TRUE(contains(refusal_of(with_required({"--settle", "t.x", "--settle-slope", bad})),
                         "expected a number of 0 or more"))
        << bad;
  }
  EXPECT_TRUE(parse(with_required({"--settle", "t.x", "--settle-slope", "0"})).has_value());
}

TEST(ExploreArgs, ASettleWindowLongerThanTheSettleMaximumIsRefusedAtOnce) {
  // The default window is 30 s: a smaller maximum alone can never settle.
  const std::string by_default =
      refusal_of(with_required({"--settle", "t.x", "--settle-max", "20s"}));
  EXPECT_TRUE(
      contains(by_default, "--settle-window (30000 ms) is longer than --settle-max (20000 ms)"))
      << by_default;
  EXPECT_TRUE(contains(refusal_of(with_required({"--settle", "t.x", "--settle-window", "301s"})),
                       "longer than --settle-max (300000 ms)"))
      << "the default maximum is 300 s";
  EXPECT_TRUE(contains(
      refusal_of(with_required({"--settle", "t.x", "--settle-window", "9s", "--settle-max", "8s"})),
      "could never settle"));
  EXPECT_TRUE(
      parse(with_required({"--settle", "t.x", "--settle-window", "8s", "--settle-max", "8s"}))
          .has_value());
}

TEST(ExploreArgs, TheSettleOptionsNeedTheSettleMetric) {
  for (const std::string_view key : {"--settle-slope", "--settle-window", "--settle-max"}) {
    EXPECT_TRUE(contains(refusal_of(with_required({key, key == "--settle-slope" ? "1" : "5s"})),
                         "need --settle METRIC"))
        << key;
  }
}

TEST(ExploreArgs, ACountIsAWholeNumberOfAtLeastOne) {
  for (const std::string_view key : {"--replicates", "--final-replicates", "--elimination",
                                     "--interleave", "--reference-replicates"}) {
    EXPECT_TRUE(contains(refusal_of(with_required({key, "0"})), "must be at least 1")) << key;
    EXPECT_TRUE(contains(refusal_of(with_required({key, "two"})), "expected a whole number"))
        << key;
    EXPECT_TRUE(contains(refusal_of(with_required({key, "5x"})), "expected a whole number"))
        << "a number with a unit is not a count: " << key;
  }
}

TEST(ExploreArgs, TheMachineFacingOptionsAreRefusedByName) {
  EXPECT_TRUE(contains(refusal_of(with_required({"--seed", "-1"})), "expected a whole number"));
  EXPECT_TRUE(contains(refusal_of(with_required({"--footprint", "lots"})), "--footprint:"));
  EXPECT_TRUE(contains(refusal_of(with_required({"--footprint", "50%"})), "absolute size"));
  EXPECT_TRUE(contains(refusal_of(with_required({"--isa", "mmx"})), "none of scalar, avx2"));
  EXPECT_TRUE(
      contains(refusal_of(with_required({"--lock", "maybe"})), "none of require, try, none"));
  for (const std::string_view policy : {"try", "none"}) {
    EXPECT_TRUE(parse(with_required({"--lock", policy})).has_value()) << policy;
  }
}

TEST(ExploreArgs, TheCeilingIsAWholeNumberOfDegreesInTheRangeAConfigurationHas) {
  EXPECT_FALSE(parse(with_required({})).value().max_temperature_c.has_value())
      << "no ceiling unless one is asked for";
  EXPECT_EQ(parse(with_required({"--max-temperature", "1"})).value().max_temperature_c, 1);
  EXPECT_EQ(parse(with_required({"--max-temperature", "150"})).value().max_temperature_c, 150);
  for (const std::string_view bad : {"0", "151"}) {
    EXPECT_TRUE(contains(refusal_of(with_required({"--max-temperature", bad})),
                         "--max-temperature: must be between 1 and 150 degrees Celsius"))
        << bad;
  }
  for (const std::string_view bad : {"hot", "90C", "-5"}) {
    EXPECT_TRUE(
        contains(refusal_of(with_required({"--max-temperature", bad})), "expected a whole number"))
        << bad;
  }
  EXPECT_FALSE(
      parse(with_required({"--max-temperature", "90", "--max-temperature", "95"})).has_value());
}

TEST(ExploreSearch, EveryCandidateAndTheReferenceShareTheDurationAndTheSettlePhase) {
  const auto parsed = parse(with_required({"--candidate", "b:fft+sort:2", "--duration", "5s",
                                           "--settle", "thermal.package", "--replicates", "2"}));
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  const explore::SearchConfig config = search_config_of(parsed.value());
  ASSERT_EQ(config.candidates.size(), 2U);
  for (const explore::Candidate& each :
       {config.candidates[0], config.candidates[1], config.reference}) {
    EXPECT_EQ(each.duration_ns, 5'000'000'000) << each.id;
    EXPECT_EQ(each.settle, parsed.value().settle) << each.id;
    EXPECT_EQ(each.duty, config.candidates[0].duty) << each.id;
  }
  EXPECT_EQ(config.base_replicates, 2U);
  EXPECT_EQ(config.objective.metric, "thermal.package");
  EXPECT_EQ(config.reference.id, "reference");
  EXPECT_EQ(config.reference.workloads, std::vector<std::string>{"sort"})
      << "the first candidate's workloads";
  EXPECT_EQ(config.reference.threads, 1U);
}

TEST(ExploreSearch, AReferenceMustRunEachOfItsWorkloadsSoItGetsAWorkerForEach) {
  const auto parsed = parse({"--objective", "a.b:max:higher", "--candidate", "x:fft+sort+graph:4"});
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(search_config_of(parsed.value()).reference.threads, 3U);
}

TEST(ExploreSearch, AReferenceTheUserNamedIsKept) {
  const auto parsed = parse(with_required({"--reference", "idle:sort:1:0"}));
  ASSERT_TRUE(parsed.has_value());
  const explore::SearchConfig config = search_config_of(parsed.value());
  EXPECT_EQ(config.reference.id, "idle");
  EXPECT_EQ(config.reference.cpus, std::vector<std::uint32_t>{0});
  EXPECT_EQ(config.reference.duration_ns, parsed.value().duration_ns);
}

// --- the report ---------------------------------------------------------------------

explore::SearchReport a_report() {
  explore::SearchReport report;
  report.objective.metric = telemetry::Metric{"thermal.package",
                                              telemetry::Unit::kCelsius,
                                              telemetry::Kind::kGauge,
                                              telemetry::Scope::kPackage,
                                              0,
                                              std::nullopt,
                                              "hwmon"};
  report.objective.statistic = GaugeStatistic::kMax;
  report.trials = 14;
  report.rounds = 2;
  explore::CandidateReport worst;
  worst.id = "wide";
  worst.fate = Fate::kSurvivor;
  worst.rounds = 2;
  worst.samples = {80.5, 81.0, 81.5};
  worst.raw = explore::estimate(std::vector<double>{80.0, 80.5, 81.0});
  worst.corrected = explore::estimate(worst.samples);
  explore::CandidateReport single;
  single.id = "narrow";
  single.fate = Fate::kEliminated;
  single.rounds = 1;
  single.samples = {70.0};
  single.raw = explore::estimate(std::vector<double>{70.0});
  single.corrected = single.raw;
  explore::CandidateReport faulty;
  faulty.id = "odd";
  faulty.fate = Fate::kFindings;
  report.candidates = {worst, single, faulty};
  report.worst_case = explore::worst_case_of("wide", worst.samples);
  report.ranking = explore::Ranking::kSeparated;
  report.references = {{1'000, 60.0}, {2'000, 60.5}};
  report.anchor = 60.0;
  report.drift = 0.5;
  report.reference = explore::estimate(std::vector<double>{60.0, 60.5});
  return report;
}

TEST(ExploreReport, SaysTheWorstCaseWithItsReplicatesSpreadAndInterval) {
  const std::string text = render_report(a_report());
  EXPECT_TRUE(contains(text,
                       "explore: objective max of thermal.package (package 0, celsius), "
                       "higher is worse\n"))
      << text;
  EXPECT_TRUE(contains(text, "explore: 14 trial(s) in 2 round(s)\n"));
  EXPECT_TRUE(contains(text, "worst case: wide: 81 +/- "));
  EXPECT_TRUE(contains(text, "(95% CI, n=3, sd 0.5"));
  EXPECT_TRUE(contains(text, "ranking: separated from the rival with the worst corrected mean\n"));
  EXPECT_FALSE(contains(text, "not compared by an interval"));
}

TEST(ExploreReport, ListsEveryReplicateOfEveryCandidateAndEveryReferenceReading) {
  const std::string text = render_report(a_report());
  EXPECT_TRUE(contains(text, "candidate wide: survivor, ranked in 2 round(s), 3 replicate(s)\n"));
  EXPECT_TRUE(contains(text, "  replicates: 80.5 81 81.5\n")) << text;
  EXPECT_TRUE(contains(text, "  corrected for drift: mean 81 +/- "));
  EXPECT_TRUE(contains(text, "  raw: mean 80.5 +/- "));
  EXPECT_TRUE(
      contains(text, "candidate narrow: eliminated, ranked in 1 round(s), 1 replicate(s)\n"));
  EXPECT_TRUE(contains(text, "mean 70 (n=1, no spread from fewer than two)"));
  EXPECT_TRUE(contains(text, "candidate odd: findings, ranked in 0 round(s), 0 replicate(s)\n"));
  EXPECT_TRUE(contains(text, "reference: anchor 60, drift 0.5"));
  EXPECT_TRUE(contains(text, "(1000, 60) (2000, 60.5)\n")) << text;
}

TEST(ExploreReport, WithoutAWorstCaseItSaysSoAndNeverAnInventedNumber) {
  explore::SearchReport report = a_report();
  report.worst_case.reset();
  report.ranking = explore::Ranking::kNone;
  const std::string text = render_report(report);
  EXPECT_TRUE(contains(text, "worst case: none; every candidate left the search with findings"));
  EXPECT_TRUE(contains(text, "ranking: no worst case\n"));
}

TEST(ExploreReport, FlagsCorrectedHardwareErrorsPerCandidateAndForTheReference) {
  explore::SearchReport clean = a_report();
  EXPECT_FALSE(contains(render_report(clean), "corrected hardware errors"));
  explore::SearchReport report = a_report();
  report.candidates[0].notes = {{false}, {true}, {true}};
  report.reference_notes = {{false}, {true}};
  report.corrected_error_trials = 3;
  const std::string text = render_report(report);
  EXPECT_TRUE(contains(text, "corrected hardware errors: 3 scored trial(s)"));
  EXPECT_TRUE(contains(text, "corrected hardware errors in replicate(s): 2 3"));
  EXPECT_TRUE(contains(text, "corrected hardware errors in reference reading(s): 2"));
}

TEST(ExploreReport, SaysWhichTrialsDidNotSettleAndWhenNoneDid) {
  EXPECT_FALSE(contains(render_report(a_report()), "settle"));
  explore::SearchReport report = a_report();
  report.candidates[0].notes = {{false, false}, {false, true}, {false, true}};
  report.reference_notes = {{false, true}, {false, false}};
  report.settle_trials = 14;
  report.unsettled_trials = 3;
  std::string text = render_report(report);
  EXPECT_TRUE(contains(text, "settle: 3 of 14 trial(s) did not settle"));
  EXPECT_FALSE(contains(text, "no trial of this search started from a controlled state"));
  EXPECT_TRUE(contains(text, "replicate(s) that did not settle: 2 3"));
  EXPECT_TRUE(contains(text, "reference reading(s) that did not settle: 1"));
  report.unsettled_trials = 14;
  text = render_report(report);
  EXPECT_TRUE(contains(text, "settle: 14 of 14 trial(s) did not settle"));
  EXPECT_TRUE(contains(text, "no trial of this search started from a controlled state"));
}

TEST(ExploreReport, NamesTheRivalAndWhoWasNotComparedByAnInterval) {
  explore::SearchReport report = a_report();
  report.rival = "narrow";
  report.uncompared = {"odd", "tiny"};
  const std::string text = render_report(report);
  EXPECT_TRUE(
      contains(text, "ranking: separated from the rival with the worst corrected mean (narrow)\n"));
  EXPECT_TRUE(contains(text,
                       "not compared by an interval (eliminated with fewer than two "
                       "replicates): odd tiny\n"));
}

TEST(ExploreReport, SaysTheSurvivorsEstimateIsFromTheReplicatesAfterItWasChosen) {
  explore::SearchReport report = a_report();
  EXPECT_FALSE(contains(render_report(report), "run after it was chosen"));
  report.candidates[0].selection_replicates = 1;
  const std::string text = render_report(report);
  EXPECT_TRUE(
      contains(text, "corrected for drift of the 2 replicate(s) run after it was chosen: mean"));
  EXPECT_TRUE(contains(text, "raw of the 2 replicate(s) run after it was chosen: mean"));
  EXPECT_TRUE(contains(text, "replicates: 80.5 | 81 81.5\n"));
  EXPECT_TRUE(contains(text, "the 1 replicate(s) before the bar chose this candidate"));
}

TEST(ExploreReport, ASearchEndedByTheReferenceSaysSoAndKeepsWhatWasMeasured) {
  explore::SearchReport report = a_report();
  report.worst_case.reset();
  report.ranking = explore::Ranking::kNone;
  report.reference_stop = results::Verdict::kFindings;
  report.candidates[0].fate = Fate::kStopped;
  const std::string text = render_report(report);
  EXPECT_TRUE(contains(text,
                       "worst case: none; the search ended early because a reference "
                       "trial's verdict was findings"))
      << text;
  EXPECT_TRUE(contains(text, "candidate wide: stopped with the search"));
  EXPECT_TRUE(contains(text, "replicates: 80.5 81 81.5")) << "what was measured is still listed";
}

TEST(ExploreReport, ACounterObjectiveNamesItsStatistic) {
  explore::SearchReport report = a_report();
  report.objective.statistic = CounterStatistic::kRate;
  EXPECT_TRUE(contains(render_report(report), "objective rate per second of thermal.package"));
}

// --- the exit codes --------------------------------------------------------------------

TEST(ExploreExit, AFindingOutranksAnUnfinishedTrialWhichOutranksASearchThatScored) {
  explore::SearchReport report = a_report();
  EXPECT_EQ(exit_code_for(report), 5) << "the faulty candidate";
  report.candidates[2].fate = Fate::kIncomplete;
  EXPECT_EQ(exit_code_for(report), 6);
  report.candidates[2].fate = Fate::kEliminated;
  EXPECT_EQ(exit_code_for(report), 0);
  report.candidates[2].fate = Fate::kFindings;
  report.candidates[1].fate = Fate::kIncomplete;
  EXPECT_EQ(exit_code_for(report), 5) << "both: the finding";
}

TEST(ExploreExit, AReferenceThatFoundSomethingIsAFindingAndOneThatDidNotFinishIsUnread) {
  explore::SearchReport report = a_report();
  report.candidates[2].fate = Fate::kEliminated;
  report.reference_stop = results::Verdict::kFindings;
  EXPECT_EQ(exit_code_for(report), 5);
  report.reference_stop = results::Verdict::kIncomplete;
  EXPECT_EQ(exit_code_for(report), 6);
  report.candidates[2].fate = Fate::kFindings;
  EXPECT_EQ(exit_code_for(report), 5) << "a finding outranks";
}

TEST(ExploreExit, CorrectedHardwareErrorsExitFiveAsStressDoes) {
  explore::SearchReport report = a_report();
  report.candidates[2].fate = Fate::kEliminated;
  ASSERT_EQ(exit_code_for(report), 0);
  report.corrected_error_trials = 1;
  EXPECT_EQ(exit_code_for(report), 5);
  EXPECT_EQ(exit_code_for(results::Verdict::kCorrectedErrors, true, true), 5)
      << "stress's mapping of the same trial";
  report.candidates[2].fate = Fate::kIncomplete;
  EXPECT_EQ(exit_code_for(report), 5) << "5 outranks 6";
}

TEST(ExploreExit, ASearchThatDidNotEndIsRefusedFailedOrUnread) {
  using explore::ExploreFault;
  for (const ExploreFault refused :
       {ExploreFault::kUnknownMetric, ExploreFault::kMetricUnavailable, ExploreFault::kStatistic,
        ExploreFault::kConfiguration, ExploreFault::kDutyCycle}) {
    EXPECT_EQ(exit_code_for(explore::make_error(refused, "x")), 1) << explore::describe(refused);
  }
  EXPECT_EQ(exit_code_for(explore::make_error(ExploreFault::kTrial, "x")), 3);
  EXPECT_EQ(exit_code_for(explore::make_error(ExploreFault::kReference, "x")), 3);
  EXPECT_EQ(exit_code_for(explore::make_error(ExploreFault::kNoReading, "x")), 6);
  EXPECT_EQ(exit_code_for(explore::make_error(static_cast<ExploreFault>(99), "x")), 1);
}

// --- the search on the controller, over the fake
// ---------------------------------------------------

/// The runner on a machine of one CPU, memory served as the kernel shows it, and a clock that
/// moves 10 ms on every read. The workers it starts exit at once, which is a trial that did
/// not finish: the real ones are tests/integration/m6.
struct Searching {
  Searching() {
    syscalls.advance_clocks_per_read(10'000'000);
    syscalls.add_tree(std::filesystem::path{LOADFORGE_SOURCE_DIR} / "tests" / "fixtures" /
                          "procfs" / "meminfo" / "present" / "proc",
                      "/proc");
    syscalls.add_tree(std::filesystem::path{LOADFORGE_SOURCE_DIR} / "tests" / "fixtures" /
                          "procfs" / "stat" / "present" / "proc",
                      "/proc");
    options.executable = "/opt/loadforge";
    options.sample_interval_ns = 30'000'000;
    auto made = ControllerTrialRunner::create(syscalls, snapshot, registry, options);
    EXPECT_TRUE(made.has_value());
    runner = std::move(made).value();
  }

  static explore::SearchConfig config(std::string_view objective,
                                      std::vector<std::string_view> more = {}) {
    std::vector<std::string_view> args{"--objective", objective, "--candidate", "a:sort:1"};
    args.insert(args.end(), more.begin(), more.end());
    const auto parsed = parse_explore_args(args);
    EXPECT_TRUE(parsed.has_value());
    return search_config_of(parsed.value());
  }

  int run(explore::SearchConfig search) {
    out.str("");
    err.str("");
    return run_explore(*runner, std::move(search), out, err);
  }

  controller_testing::MachineTree tree{controller_testing::MachineShape{1, 64, true}};
  topology::Snapshot snapshot = tree.accepted();
  testing::FakeSyscalls syscalls;
  workloads::BuiltinRegistry registry;
  TrialOptions options;
  std::unique_ptr<ControllerTrialRunner> runner;
  std::ostringstream out;
  std::ostringstream err;
};

TEST(ExploreRun, ATrialThatCannotStartEndsTheSearchWithTheKernelsReason) {
  Searching search;
  search.syscalls.script_forks({{0, EAGAIN}});
  EXPECT_EQ(search.run(Searching::config("memory.available:min:lower")), 3);
  EXPECT_TRUE(contains(search.err.str(), "loadforge: explore: trial failed: ")) << search.err.str();
  EXPECT_TRUE(contains(search.err.str(), "fork")) << search.err.str();
  EXPECT_TRUE(search.out.str().empty()) << "no report of a search that did not end";
}

TEST(ExploreRun, AnUnrunnableTrialEndsTheSearchNamingTheCandidateAndWhy) {
  Searching search;
  explore::SearchConfig config = Searching::config("memory.available:min:lower");
  config.reference.workloads = {"no_such_workload"};  // the first to run
  EXPECT_EQ(search.run(std::move(config)), 3);
  EXPECT_TRUE(contains(search.err.str(), "no workload named 'no_such_workload'"))
      << search.err.str();
  EXPECT_EQ(search.syscalls.fork_count(), 0);
}

TEST(ExploreRun, AReferenceThatDidNotFinishCannotMeasureDriftAndEndsTheSearchWithAReport) {
  Searching search;
  search.syscalls.script_forks({{1001, 0}});
  search.syscalls.script_nonblocking_reaps({{platform::Reaped{1001, 0}, 0}});
  EXPECT_EQ(search.run(Searching::config("memory.available:min:lower")), 6);
  EXPECT_TRUE(
      contains(search.out.str(), "the search ended early because a reference trial's verdict was"))
      << search.out.str();
  EXPECT_TRUE(contains(search.out.str(), ": stopped with the search"));
  EXPECT_EQ(search.err.str(), "");
}

TEST(ExploreRun, AWorkerWhoseExecFailedEndsTheSearchWithItsExitCodeAndSaysNothing) {
  Searching search;
  search.syscalls.script_forks({{0, 0}});
  search.syscalls.set_parent_pid(testing::FakeSyscalls::kFakePid);
  EXPECT_EQ(search.run(Searching::config("memory.available:min:lower")), 120);
  EXPECT_TRUE(search.err.str().empty()) << search.err.str();
  EXPECT_TRUE(search.out.str().empty());
  EXPECT_EQ(search.runner->worker_exit_code(), 120);
}

TEST(ExploreRun, ASearchThatIsNotWellFormedIsRefusedBeforeAnyWorkerStarts) {
  Searching search;
  explore::SearchConfig config = Searching::config("memory.available:min:lower");
  config.reference.id = "a";  // the candidate's
  EXPECT_EQ(search.run(std::move(config)), 1);
  EXPECT_TRUE(contains(search.err.str(), "search not well formed")) << search.err.str();
  EXPECT_EQ(search.syscalls.fork_count(), 0);
}

TEST(ExploreRun, AnObjectiveThisMachineCannotReadIsRefusedWithTheSourcesRemedyAndNothingStarts) {
  Searching search;
  EXPECT_EQ(search.run(Searching::config("thermal.package:max:higher")), 1);
  EXPECT_TRUE(contains(search.err.str(), "objective metric unavailable")) << search.err.str();
  EXPECT_TRUE(contains(search.err.str(), "hwmon")) << "the remedy: " << search.err.str();
  EXPECT_EQ(search.syscalls.fork_count(), 0);
  EXPECT_EQ(search.runner->trials_started(), 0U);
}

TEST(ExploreRun, ASettleMetricThisMachineCannotReadIsRefusedTheSameWay) {
  Searching search;
  EXPECT_EQ(
      search.run(Searching::config("memory.available:min:lower", {"--settle", "thermal.package"})),
      1);
  EXPECT_TRUE(contains(search.err.str(), "settle metric thermal.package")) << search.err.str();
  EXPECT_EQ(search.syscalls.fork_count(), 0);
}

// --- the command
// ------------------------------------------------------------------------------------

struct Invoked {
  int code;
  std::string out;
  std::string err;
};

Invoked command(std::vector<std::string_view> args, const std::string& self_link,
                std::string_view sysfs, const TrialOptions& base) {
  std::ostringstream out;
  std::ostringstream err;
  const int code = explore_command(args, self_link, sysfs, base, out, err);
  return {code, out.str(), err.str()};
}

TEST(ExploreCommand, DispatchedFromTheCommandLineAndUsageIsStated) {
  std::ostringstream out;
  std::ostringstream err;
  const std::vector<std::string_view> missing{"explore"};
  EXPECT_EQ(run(missing, out, err), 2);
  EXPECT_TRUE(contains(err.str(), "loadforge: explore: --objective is required")) << err.str();
  std::ostringstream help;
  const std::vector<std::string_view> asked{"--help"};
  EXPECT_EQ(run(asked, help, err), 0);
  EXPECT_TRUE(contains(help.str(), "loadforge explore --objective METRIC")) << help.str();
}

TEST(ExploreCommand, AMisusedOptionIsAUsageError) {
  const Invoked ran = command({"--objective", "m.x:max:sideways", "--candidate", "a:sort:1"},
                              "/proc/self/exe", "/sys", TrialOptions{});
  EXPECT_EQ(ran.code, 2);
  EXPECT_TRUE(contains(ran.err, "neither higher nor lower")) << ran.err;
}

TEST(ExploreCommand, ARefusalToNameTheExecutableOrDescribeTheMachineHappensBeforeAnyWorker) {
  const std::vector<std::string_view> args{"--objective", "memory.available:min:lower",
                                           "--candidate", "a:sort:1"};
  Invoked ran = command(args, "/nonexistent/loadforge", "/sys", TrialOptions{});
  EXPECT_EQ(ran.code, 1);
  EXPECT_TRUE(contains(ran.err, "cannot resolve this executable")) << ran.err;
  ran = command(args, "/proc/self/exe", "/nonexistent/sys", TrialOptions{});
  EXPECT_EQ(ran.code, 1);
  EXPECT_TRUE(contains(ran.err, "cannot describe this machine")) << ran.err;
}

std::vector<std::unique_ptr<telemetry::Source>> no_sources(platform::Syscalls& /*s*/,
                                                           platform::FileSystem& /*f*/,
                                                           const MachineRoots& /*r*/) {
  return {};
}

TEST(ExploreCommand, TelemetryThatCannotBeSetUpRefusesTheCommand) {
  struct Twin final : telemetry::Source {
    [[nodiscard]] std::string_view name() const noexcept override { return "twin"; }
    std::vector<telemetry::MetricStatus> probe() override {
      return {{telemetry::Metric{"twin.value", telemetry::Unit::kCount, telemetry::Kind::kGauge,
                                 telemetry::Scope::kMachine, 0, std::nullopt, "test"},
               std::nullopt}};
    }
    void sample(std::vector<telemetry::Reading>& out) override {
      out.emplace_back(telemetry::Gauge{1});
    }
  };
  TrialOptions base;
  base.sources = [](platform::Syscalls& /*s*/, platform::FileSystem& /*f*/,
                    const MachineRoots& /*r*/) {
    std::vector<std::unique_ptr<telemetry::Source>> sources;
    sources.push_back(std::make_unique<Twin>());
    sources.push_back(std::make_unique<Twin>());
    return sources;
  };
  const Invoked ran = command({"--objective", "twin.value:max:higher", "--candidate", "a:sort:1"},
                              "/proc/self/exe", "/sys", base);
  EXPECT_EQ(ran.code, 1);
  EXPECT_TRUE(contains(ran.err, "cannot set up telemetry: duplicate series")) << ran.err;
}

TEST(ExploreCommand, AMachineWithNothingToReadStatesItAndRefusesTheObjectiveBeforeAnyWorker) {
  TrialOptions base;
  base.sources = no_sources;
  const Invoked ran =
      command({"--objective", "thermal.package:max:higher", "--candidate", "a:sort:1", "--verbose"},
              "/proc/self/exe", "/sys", base);
  EXPECT_EQ(ran.code, 1);
  EXPECT_TRUE(contains(ran.err, "objective metric unknown")) << ran.err;
  EXPECT_TRUE(contains(ran.out, "telemetry capability: 0 of 0 metric(s) present")) << ran.out;
}

TEST(ExploreCommand, ACeilingNothingCanBeReadForRefusesTheCommandBeforeTheFirstTrial) {
  TrialOptions base;
  base.sources = no_sources;
  const Invoked ran = command({"--objective", "memory.available:min:lower", "--candidate",
                               "a:sort:1", "--max-temperature", "85"},
                              "/proc/self/exe", "/sys", base);
  EXPECT_EQ(ran.code, 1);
  EXPECT_TRUE(contains(ran.err,
                       "loadforge: explore: --max-temperature is 85 C and no temperature "
                       "can be read, so the ceiling could not be enforced: no "
                       "temperature sensor was offered; leave the option out"))
      << ran.err;
  EXPECT_TRUE(ran.out.empty()) << "nothing was probed for the user to read, nothing ran";
}

}  // namespace
}  // namespace loadforge::cli
