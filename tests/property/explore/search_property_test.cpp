// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for src/explore: properties of the search under a mocked runner. Each
// runs over a fixed set of seeds, so a failure names the seed that makes it and
// reproduces; the generator is a small splitmix64 written out here because the
// standard distributions are not the same bits on every library.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#include "explore/search.hpp"
#include "unit/explore/model_runner.hpp"

namespace loadforge::explore {
namespace {

using explore_test::make_candidate;
using explore_test::ModelRunner;
using results::Verdict;

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  // Uniform in [0, 1).
  double unit() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }
  // Uniform in [-1, 1).
  double centred() { return 2.0 * unit() - 1.0; }
  std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

 private:
  std::uint64_t state_;
};

constexpr std::uint64_t kSeeds = 120;

std::vector<std::string> ids(std::size_t n) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back("c" + std::to_string(i));
  }
  return out;
}

SearchConfig random_config(Rng& rng, std::size_t n) {
  SearchConfig config;
  config.objective.metric = "thermal.package";
  config.reference = make_candidate("ref");
  for (const std::string& id : ids(n)) {
    config.candidates.push_back(make_candidate(id));
  }
  config.base_replicates = 1 + rng.below(3);
  config.elimination_factor = 2 + rng.below(2);
  config.final_replicates = 2 + rng.below(4);
  config.interleave_every = 1 + rng.below(6);
  config.reference_replicates = 1 + rng.below(3);
  return config;
}

// The mock machine: value = base[id] + drift(t) + noise, the reference
// included. `drift` is the machine warming (or cooling) over the search.
struct Machine {
  std::map<std::string, double> base;
  double drift_per_ms = 0.0;
  double noise = 0.0;
  Rng rng;
  explicit Machine(std::uint64_t seed) : rng(seed) {}
  double value(const Candidate& c, std::int64_t mid_ns) {
    const double noise_draw = noise > 0.0 ? noise * rng.centred() : 0.0;
    return base.at(c.id) + drift_per_ms * static_cast<double>(mid_ns) / 1.0e6 + noise_draw;
  }
};

core::Result<SearchReport, ExploreError> run(Machine& machine, const SearchConfig& config) {
  ModelRunner runner(
      [&](const Candidate& c, std::int64_t mid, std::size_t) { return machine.value(c, mid); });
  return explore(runner, config);
}

TEST(SearchProperty, AConstantObjectiveRanksNothing) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    Rng rng(seed);
    const std::size_t n = 2 + rng.below(8);
    const SearchConfig config = random_config(rng, n);
    Machine machine(seed);
    const double level = 20.0 + 60.0 * rng.unit();
    machine.base["ref"] = 5.0;
    for (const std::string& id : ids(n)) {
      machine.base[id] = level;
    }
    const auto report = run(machine, config);
    ASSERT_TRUE(report.has_value()) << "seed " << seed << ": " << report.error().detail;
    EXPECT_EQ(report.value().ranking, Ranking::kIndistinguishable) << "seed " << seed;
    ASSERT_TRUE(report.value().worst_case.has_value()) << "seed " << seed;
    EXPECT_NEAR(report.value().worst_case->mean, level, 1e-9) << "seed " << seed;
    EXPECT_EQ(report.value().worst_case->candidate, "c0")
        << "seed " << seed << ": ties go to the first";
  }
}

TEST(SearchProperty, AConstantObjectiveSeparatesNothingEvenWithNoise) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    Rng rng(seed);
    const std::size_t n = 2 + rng.below(6);
    SearchConfig config = random_config(rng, n);
    config.base_replicates = 3;  // enough for rivals to carry an interval
    Machine machine(seed);
    machine.noise = 2.0;
    machine.base["ref"] = 5.0;
    for (const std::string& id : ids(n)) {
      machine.base[id] = 50.0;
    }
    const auto report = run(machine, config);
    ASSERT_TRUE(report.has_value());
    EXPECT_NE(report.value().ranking, Ranking::kSeparated) << "seed " << seed;
  }
}

TEST(SearchProperty, DriftInjectedIsCorrectedAndTheTrueWorstCaseIsFound) {
  std::size_t drifted = 0;
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    Rng rng(seed);
    const std::size_t n = 2 + rng.below(7);
    const SearchConfig config = random_config(rng, n);
    Machine machine(seed);
    // A drift of several degrees per trial slot against candidates 1.5 apart:
    // uncorrected, the later trials would outrank the better candidates.
    machine.drift_per_ms = (rng.below(2) == 0 ? 1.0 : -1.0) * (1.0 + 3.0 * rng.unit());
    machine.base["ref"] = 10.0 + 10.0 * rng.unit();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    for (std::size_t i = n; i > 1; --i) {
      std::swap(order[i - 1], order[rng.below(i)]);
    }
    const std::vector<std::string> names = ids(n);
    for (std::size_t i = 0; i < n; ++i) {
      machine.base[names[i]] = 40.0 + 1.5 * static_cast<double>(order[i]);
    }
    const std::string truth = names[static_cast<std::size_t>(
        std::max_element(order.begin(), order.end()) - order.begin())];
    const auto report = run(machine, config);
    ASSERT_TRUE(report.has_value()) << "seed " << seed << ": " << report.error().detail;
    EXPECT_EQ(report.value().worst_case->candidate, truth) << "seed " << seed;
    // A linear drift is interpolated exactly: every corrected replicate of a
    // candidate is its base plus one shared constant, whatever the time.
    const double offset = report.value().anchor - machine.base["ref"];
    for (const CandidateReport& entry : report.value().candidates) {
      for (const double sample : entry.samples) {
        EXPECT_NEAR(sample - machine.base[entry.id], offset, 1e-6)
            << "seed " << seed << " " << entry.id;
      }
    }
    if (std::abs(report.value().drift) > 3.0) {
      ++drifted;
    }
  }
  EXPECT_GT(drifted, kSeeds / 2) << "the injected drift must be large enough to matter";
}

TEST(SearchProperty, TheWorstCaseIsFoundUnderDriftAndNoiseWhenGapsDwarfBoth) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    Rng rng(seed);
    const std::size_t n = 3 + rng.below(6);
    SearchConfig config = random_config(rng, n);
    config.base_replicates = 2;
    Machine machine(seed);
    machine.drift_per_ms = rng.centred() * 2.0;
    machine.noise = 0.4;
    machine.base["ref"] = 15.0;
    const std::vector<std::string> names = ids(n);
    const std::size_t worst = rng.below(n);
    for (std::size_t i = 0; i < n; ++i) {
      machine.base[names[i]] = i == worst ? 90.0 : 40.0 + 2.0 * static_cast<double>(i);
    }
    const auto report = run(machine, config);
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report.value().worst_case->candidate, names[worst]) << "seed " << seed;
    EXPECT_EQ(report.value().ranking, Ranking::kSeparated) << "seed " << seed;
  }
}

TEST(SearchProperty, MoreReplicatesNarrowTheInterval) {
  const auto mean_width = [](std::size_t replicates) {
    double total = 0.0;
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
      Rng rng(seed);
      SearchConfig config = random_config(rng, 1);
      config.final_replicates = replicates;
      Machine machine(seed * 7919);
      machine.noise = 3.0;
      machine.base["ref"] = 10.0;
      machine.base["c0"] = 60.0;
      const auto report = run(machine, config);
      total += report.value().worst_case->half_width;
    }
    return total / static_cast<double>(kSeeds);
  };
  const double narrow = mean_width(16);
  const double wide = mean_width(4);
  EXPECT_LT(narrow, 0.5 * wide) << "16 replicates: " << narrow << ", 4 replicates: " << wide;
}

TEST(SearchProperty, TheIntervalCoversTheTrueMeanAboutNinetyFivePercentOfTheTime) {
  std::size_t covered = 0;
  constexpr std::uint64_t kRuns = 600;
  for (std::uint64_t seed = 1; seed <= kRuns; ++seed) {
    Rng rng(seed);
    SearchConfig config = random_config(rng, 1);
    config.final_replicates = 5;
    config.base_replicates = 1;
    Machine machine(seed * 104729);
    machine.noise = 4.0;
    machine.base["ref"] = 10.0;
    machine.base["c0"] = 60.0;
    const auto report = run(machine, config);
    const WorstCase& claim = *report.value().worst_case;
    if (std::abs(claim.mean - 60.0) <= claim.half_width) {
      ++covered;
    }
  }
  const double rate = static_cast<double>(covered) / static_cast<double>(kRuns);
  // Nominally 95%. The reference's noise is propagated conservatively, so a
  // little over is expected and a shortfall is the failure being guarded.
  EXPECT_GT(rate, 0.93);
  EXPECT_LT(rate, 0.995);
}

TEST(SearchProperty, AWorstCaseIsNeverReportedWithoutItsReplicatesAndSpread) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    Rng rng(seed);
    const std::size_t n = 1 + rng.below(8);
    const SearchConfig config = random_config(rng, n);
    Machine machine(seed);
    machine.noise = 1.0;
    machine.base["ref"] = 10.0;
    for (const std::string& id : ids(n)) {
      machine.base[id] = 40.0 + 10.0 * rng.unit();
    }
    // Candidates that find something are lost mid-search at random.
    ModelRunner runner(
        [&](const Candidate& c, std::int64_t mid, std::size_t) { return machine.value(c, mid); });
    std::size_t lost_budget = rng.below(3);
    runner.verdict = [&](const Candidate& c, std::size_t call) {
      if (c.id != "ref" && call % 11 == 5 && lost_budget > 0) {
        --lost_budget;
        return Verdict::kFindings;
      }
      return Verdict::kClean;
    };
    const auto report = explore(runner, config);
    ASSERT_TRUE(report.has_value()) << "seed " << seed;
    if (!report.value().worst_case.has_value()) {
      EXPECT_EQ(report.value().ranking, Ranking::kNone);
      continue;
    }
    const WorstCase& claim = *report.value().worst_case;
    EXPECT_GE(claim.replicates, 2U) << "seed " << seed;
    EXPECT_GE(claim.replicates, config.final_replicates);
    EXPECT_TRUE(std::isfinite(claim.stddev));
    EXPECT_TRUE(std::isfinite(claim.half_width));
    // Never narrower than the replicates' own spread allows, and the reference's
    // noise is part of it, never hidden.
    EXPECT_GE(claim.half_width, t_critical(claim.replicates - 1) * claim.stddev /
                                        std::sqrt(static_cast<double>(claim.replicates)) -
                                    1e-9);
    EXPECT_GE(claim.correction_stddev, 0.0);
    const std::string text = format(claim);
    EXPECT_NE(text.find("n=" + std::to_string(claim.replicates)), std::string::npos);
    EXPECT_NE(text.find("sd "), std::string::npos);
    EXPECT_NE(text.find("95% CI"), std::string::npos);
  }
}

TEST(SearchProperty, TheReferenceBracketsEveryCandidateTrialAndIsNeverFarApart) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    Rng rng(seed);
    const std::size_t n = 1 + rng.below(8);
    const SearchConfig config = random_config(rng, n);
    Machine machine(seed);
    machine.noise = 1.0;
    machine.base["ref"] = 10.0;
    for (const std::string& id : ids(n)) {
      machine.base[id] = 40.0 + 10.0 * rng.unit();
    }
    ModelRunner runner(
        [&](const Candidate& c, std::int64_t mid, std::size_t) { return machine.value(c, mid); });
    const auto report = explore(runner, config);
    ASSERT_TRUE(report.has_value());
    const std::vector<std::string>& log = runner.ran;
    ASSERT_GE(log.size(), config.reference_replicates + 1);
    for (std::size_t i = 0; i < config.reference_replicates; ++i) {
      EXPECT_EQ(log[i], "ref") << "seed " << seed << ": the anchor comes first";
    }
    EXPECT_EQ(log.back(), "ref") << "seed " << seed << ": the last trial is a reference";
    std::size_t run_of_candidates = 0;
    for (const std::string& id : log) {
      run_of_candidates = id == "ref" ? 0 : run_of_candidates + 1;
      EXPECT_LE(run_of_candidates, config.interleave_every) << "seed " << seed;
    }
    EXPECT_EQ(log.size(), report.value().trials);
  }
}

TEST(SearchProperty, OneHistoryGivesOneReport) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    Rng rng(seed);
    const std::size_t n = 1 + rng.below(8);
    const SearchConfig config = random_config(rng, n);
    const auto go = [&] {
      Machine machine(seed);
      machine.noise = 2.0;
      machine.drift_per_ms = 0.5;
      machine.base["ref"] = 10.0;
      for (const std::string& id : ids(n)) {
        machine.base[id] = 30.0 + static_cast<double>(id.back() - '0') * 3.0;
      }
      return run(machine, config);
    };
    const auto first = go();
    const auto second = go();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first.value().trials, second.value().trials);
    EXPECT_TRUE(first.value().worst_case->mean == second.value().worst_case->mean);
    EXPECT_TRUE(first.value().worst_case->half_width == second.value().worst_case->half_width);
    EXPECT_EQ(first.value().worst_case->candidate, second.value().worst_case->candidate);
  }
}

TEST(SearchProperty, EverySurvivorOfARoundHasTheSameNumberOfReplicates) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    Rng rng(seed);
    const std::size_t n = 2 + rng.below(8);
    const SearchConfig config = random_config(rng, n);
    Machine machine(seed);
    machine.noise = 1.0;
    machine.base["ref"] = 10.0;
    for (const std::string& id : ids(n)) {
      machine.base[id] = 40.0 + 10.0 * rng.unit();
    }
    const auto report = run(machine, config);
    ASSERT_TRUE(report.has_value());
    for (const CandidateReport& entry : report.value().candidates) {
      // base * factor^(rounds survived - 1) replicates, topped up for the winner.
      std::size_t expected = config.base_replicates;
      for (std::size_t r = 1; r < entry.rounds; ++r) {
        expected *= config.elimination_factor;
      }
      if (entry.fate == Fate::kSurvivor) {
        EXPECT_GE(entry.samples.size(), std::max(expected, config.final_replicates));
      } else {
        EXPECT_EQ(entry.samples.size(), expected) << "seed " << seed << " " << entry.id;
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::explore
