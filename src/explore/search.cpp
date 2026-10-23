// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore/search.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace loadforge::explore {

namespace {

using results::Verdict;

constexpr bool unscored(Verdict verdict) noexcept {
  return verdict == Verdict::kFindings || verdict == Verdict::kIncomplete;
}

std::optional<std::string> parameter_problem(const SearchConfig& config) {
  if (config.candidates.empty()) {
    return "no candidates";
  }
  if (config.base_replicates < 1) {
    return "base_replicates must be at least 1";
  }
  if (config.elimination_factor < 2) {
    return "elimination_factor must be at least 2";
  }
  if (config.final_replicates < 2) {
    return "final_replicates must be at least 2: a worst case needs a variance";
  }
  if (config.interleave_every < 1) {
    return "interleave_every must be at least 1";
  }
  if (config.reference_replicates < 1) {
    return "reference_replicates must be at least 1";
  }
  return std::nullopt;
}

std::optional<std::string> identity_problem(const SearchConfig& config) {
  if (config.reference.id.empty()) {
    return "the reference has no id";
  }
  std::set<std::string> seen;
  seen.insert(config.reference.id);
  for (const Candidate& candidate : config.candidates) {
    if (candidate.id.empty()) {
      return "a candidate has no id";
    }
    if (!seen.insert(candidate.id).second) {
      return "candidate id " + candidate.id + " is used twice, or is the reference's";
    }
  }
  return std::nullopt;
}

// A settle phase is measured on a gauge this machine can read, or the search
// is refused before it starts -- the trial would refuse it too, hours in.
std::optional<ExploreError> settle_problem(const Candidate& candidate,
                                           const telemetry::CapabilityReport& capabilities) {
  if (!candidate.settle.has_value()) {
    return std::nullopt;
  }
  const std::string& name = candidate.settle->metric;
  if (candidate.settle->window_ns > candidate.settle->max_ns) {
    // The slope is judged once a full window has been read, and the phase ends at its
    // maximum: such a phase can never settle.
    return make_error(ExploreFault::kConfiguration,
                      "settle window of " + candidate.id + " is longer than its settle maximum");
  }
  for (const telemetry::SourceStatus& source : capabilities.sources) {
    for (const telemetry::MetricStatus& status : source.metrics) {
      if (status.metric.name != name) {
        continue;
      }
      if (status.unavailable.has_value()) {
        return make_error(ExploreFault::kMetricUnavailable,
                          "settle metric " + name + " of " + candidate.id +
                              " is unavailable: " + status.unavailable->detail + " (" +
                              status.unavailable->remediation + ")");
      }
      if (status.metric.kind != telemetry::Kind::kGauge) {
        return make_error(ExploreFault::kConfiguration,
                          "settle metric " + name + " of " + candidate.id + " is not a gauge");
      }
      return std::nullopt;
    }
  }
  return make_error(ExploreFault::kUnknownMetric, "settle metric " + name + " of " + candidate.id +
                                                      " is not in the capability report");
}

struct Measured {
  Verdict verdict = Verdict::kIncomplete;
  TrialNote note;
  std::int64_t at_ns = 0;
  double value = 0.0;
};

struct Observation {
  std::int64_t at_ns = 0;
  double raw = 0.0;
  TrialNote note;
};

struct State {
  bool alive = true;
  Fate fate = Fate::kSurvivor;
  std::size_t rounds = 0;
  std::vector<Observation> observations;
  /// For the survivor: how many of its observations were taken before it was chosen.
  std::size_t selection_observations = 0;
};

// One search in progress. Everything here is a plain sequence of trials on the
// caller's thread; the order of runs is fixed by the configuration, so a
// deterministic runner gives a deterministic report.
class Execution {
 public:
  Execution(SearchConfig config, BoundObjective bound, TrialRunner& runner)
      : config_(std::move(config)),
        bound_(std::move(bound)),
        runner_(&runner),
        states_(config_.candidates.size()) {}

  core::Result<SearchReport, ExploreError> run() {
    if (auto anchored = anchor(); !anchored) {
      return anchored.error();
    }
    std::size_t target = config_.base_replicates;
    while (alive_count() > 1) {
      if (auto ran = passes(target); !ran) {
        return ran.error();
      }
      if (auto closed = close_reference(); !closed) {
        return closed.error();
      }
      eliminate();
      target *= config_.elimination_factor;
      ++rounds_;
    }
    if (auto ran = fresh_replicates(); !ran) {
      return ran.error();
    }
    if (auto closed = close_reference(); !closed) {
      return closed.error();
    }
    return report();
  }

 private:
  core::Result<Measured, ExploreError> measure(const Candidate& candidate) {
    auto outcome = runner_->run(candidate);
    ++trials_;
    if (!outcome) {
      std::string detail = candidate.id + ": " + outcome.error().detail;
      ExploreError error = make_error(ExploreFault::kTrial, std::move(detail));
      error.trial = outcome.error().fault;
      return error;
    }
    const TrialOutcome& trial = outcome.value();
    const bool asked = candidate.settle.has_value();
    settle_trials_ += static_cast<std::size_t>(asked);
    unsettled_trials_ += static_cast<std::size_t>(asked && !trial.settled);
    const statistics::Window window = trial.signature.window;
    Measured measured;
    measured.verdict = trial.verdict;
    measured.note.corrected_errors = trial.verdict == Verdict::kCorrectedErrors;
    measured.note.unsettled = asked && !trial.settled;
    measured.at_ns = window.start_ns + (window.end_ns - window.start_ns) / 2;
    if (unscored(trial.verdict)) {
      return measured;
    }
    auto value = score(bound_, trial.signature);
    if (!value) {
      return value.error();
    }
    measured.value = value.value();
    return measured;
  }

  core::Result<core::Ok, ExploreError> reference_trial() {
    auto measured = measure(config_.reference);
    if (!measured) {
      return measured.error();
    }
    if (unscored(measured.value().verdict)) {
      return make_error(ExploreFault::kReference,
                        "the reference " + config_.reference.id + " ended " +
                            std::string(results::describe(measured.value().verdict)));
    }
    references_.push_back({measured.value().at_ns, measured.value().value});
    reference_notes_.push_back(measured.value().note);
    since_reference_ = 0;
    return core::Ok{};
  }

  core::Result<core::Ok, ExploreError> anchor() {
    for (std::size_t i = 0; i < config_.reference_replicates; ++i) {
      if (auto ran = reference_trial(); !ran) {
        return ran.error();
      }
    }
    std::vector<double> values;
    values.reserve(references_.size());
    for (const ReferencePoint& point : references_) {
      values.push_back(point.value);
    }
    anchor_ = estimate(values).mean;
    return core::Ok{};
  }

  core::Result<core::Ok, ExploreError> close_reference() {
    if (since_reference_ == 0) {
      return core::Ok{};
    }
    return reference_trial();
  }

  core::Result<core::Ok, ExploreError> candidate_trial(std::size_t which) {
    State& state = states_[which];
    auto measured = measure(config_.candidates[which]);
    if (!measured) {
      return measured.error();
    }
    const Measured& result = measured.value();
    if (unscored(result.verdict)) {
      state.alive = false;
      state.fate = result.verdict == Verdict::kFindings ? Fate::kFindings : Fate::kIncomplete;
      return core::Ok{};
    }
    state.observations.push_back({result.at_ns, result.value, result.note});
    ++since_reference_;
    if (since_reference_ >= config_.interleave_every) {
      return reference_trial();
    }
    return core::Ok{};
  }

  // Pass p runs every survivor once, in configured order. Survivors therefore
  // have the same number of replicates at any pass boundary.
  core::Result<core::Ok, ExploreError> passes(std::size_t target) {
    for (; pass_ < target; ++pass_) {
      for (std::size_t which = 0; which < states_.size(); ++which) {
        if (!states_[which].alive) {
          continue;
        }
        if (auto ran = candidate_trial(which); !ran) {
          return ran.error();
        }
      }
    }
    return core::Ok{};
  }

  // The survivor, chosen on the replicates it has, is run final_replicates more times.
  // A mean and an interval from the replicates that chose it would be biased towards
  // the worst (the best of several noisy candidates is high by chance), so the
  // reported claim comes from these fresh ones alone. With one candidate nothing
  // chose it and they are all its replicates.
  core::Result<core::Ok, ExploreError> fresh_replicates() {
    const auto survivor =
        std::ranges::find_if(states_, [](const State& state) { return state.alive; });
    if (survivor == states_.end()) {
      return core::Ok{};
    }
    survivor->selection_observations = survivor->observations.size();
    const auto which = static_cast<std::size_t>(survivor - states_.begin());
    for (std::size_t fresh = 0; fresh < config_.final_replicates && survivor->alive; ++fresh) {
      if (auto ran = candidate_trial(which); !ran) {
        return ran.error();
      }
    }
    return core::Ok{};
  }

  [[nodiscard]] std::size_t alive_count() const {
    return static_cast<std::size_t>(
        std::count_if(states_.begin(), states_.end(), [](const State& s) { return s.alive; }));
  }

  // The observations from index `from` on, drift-corrected.
  [[nodiscard]] std::vector<double> corrected_samples(const State& state,
                                                      std::size_t from = 0) const {
    std::vector<double> out;
    out.reserve(state.observations.size() - from);
    for (std::size_t i = from; i < state.observations.size(); ++i) {
      const Observation& observation = state.observations[i];
      out.push_back(
          corrected(observation.raw, baseline_at(references_, observation.at_ns), anchor_));
    }
    return out;
  }

  // What the reference's noise adds to the uncertainty of this candidate's
  // mean (drift.hpp): zero when the reference showed none.
  [[nodiscard]] Correction correction_of(const State& state, std::size_t from = 0) const {
    std::vector<std::int64_t> times;
    times.reserve(state.observations.size() - from);
    for (std::size_t i = from; i < state.observations.size(); ++i) {
      times.push_back(state.observations[i].at_ns);
    }
    Correction out;
    out.variance = correction_gain(references_, times, config_.reference_replicates) *
                   reference_variance(references_);
    out.max_degrees = reference_degrees(references_.size());
    return out;
  }

  // Keeps the worse ceil(n / factor) by corrected mean; ties keep the earlier
  // candidate, so one history gives one ranking.
  void eliminate() {
    const double direction = sign_of(bound_.direction);
    std::vector<std::pair<double, std::size_t>> ranked;
    for (std::size_t which = 0; which < states_.size(); ++which) {
      if (states_[which].alive) {
        ++states_[which].rounds;
        const std::vector<double> samples = corrected_samples(states_[which]);
        ranked.emplace_back(direction * estimate(samples).mean, which);
      }
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    const std::size_t keep =
        (ranked.size() + config_.elimination_factor - 1) / config_.elimination_factor;
    for (std::size_t rank = keep; rank < ranked.size(); ++rank) {
      State& out = states_[ranked[rank].second];
      out.alive = false;
      out.fate = Fate::kEliminated;
    }
  }

  [[nodiscard]] SearchReport report() const {
    SearchReport out;
    out.objective = bound_;
    out.references = references_;
    out.reference_notes = reference_notes_;
    out.corrected_error_trials = static_cast<std::size_t>(
        std::count_if(reference_notes_.begin(), reference_notes_.end(),
                      [](const TrialNote& note) { return note.corrected_errors; }));
    out.anchor = anchor_;
    out.trials = trials_;
    out.settle_trials = settle_trials_;
    out.unsettled_trials = unsettled_trials_;
    out.rounds = rounds_;
    std::vector<double> reference_values;
    reference_values.reserve(references_.size());
    for (const ReferencePoint& point : references_) {
      reference_values.push_back(point.value);
    }
    out.reference = estimate(reference_values);
    out.drift = references_.back().value - anchor_;
    std::optional<std::size_t> winner;
    for (std::size_t which = 0; which < states_.size(); ++which) {
      const State& state = states_[which];
      CandidateReport entry;
      entry.id = config_.candidates[which].id;
      entry.fate = state.fate;
      entry.rounds = state.rounds;
      entry.samples = corrected_samples(state);
      // The survivor's estimates are from the replicates that did not choose it.
      const std::size_t from = state.alive ? state.selection_observations : 0;
      entry.selection_replicates = from;
      std::vector<double> raw;
      raw.reserve(state.observations.size());
      for (std::size_t i = 0; i < state.observations.size(); ++i) {
        const Observation& observation = state.observations[i];
        if (i >= from) {
          raw.push_back(observation.raw);
        }
        entry.notes.push_back(observation.note);
        out.corrected_error_trials += static_cast<std::size_t>(observation.note.corrected_errors);
      }
      entry.raw = estimate(raw);
      entry.corrected = estimate(corrected_samples(state, from), correction_of(state, from));
      if (state.alive) {
        winner = which;
      }
      out.candidates.push_back(std::move(entry));
    }
    if (winner.has_value()) {
      const CandidateReport& best = out.candidates[*winner];
      const State& chosen = states_[*winner];
      out.worst_case =
          worst_case_of(best.id, corrected_samples(chosen, chosen.selection_observations),
                        correction_of(chosen, chosen.selection_observations));
      out.ranking = rank_against_rivals(out, *winner);
    }
    return out;
  }

  // The best eliminated rival -- worst by corrected mean, earlier on a tie --
  // against the winner, at the evidence each has.
  [[nodiscard]] Ranking rank_against_rivals(SearchReport& built, std::size_t winner) const {
    const double direction = sign_of(bound_.direction);
    const CandidateReport* rival = nullptr;
    for (const CandidateReport& entry : built.candidates) {
      if (entry.fate != Fate::kEliminated) {
        continue;
      }
      if (entry.samples.size() < 2) {
        built.uncompared.push_back(entry.id);
      }
      if (rival == nullptr ||
          direction * entry.corrected.mean > direction * rival->corrected.mean) {
        rival = &entry;
      }
    }
    if (rival == nullptr) {
      return Ranking::kSole;
    }
    built.rival = rival->id;
    const Estimate& best = built.candidates[winner].corrected;
    const Estimate& other = rival->corrected;
    const double best_width = best.half_width.value_or(0.0);
    if (!other.half_width.has_value()) {
      return Ranking::kIndistinguishable;
    }
    const double gap = direction * (best.mean - other.mean);
    return gap > best_width + *other.half_width ? Ranking::kSeparated : Ranking::kIndistinguishable;
  }

  SearchConfig config_;
  BoundObjective bound_;
  TrialRunner* runner_;
  std::vector<State> states_;
  std::vector<ReferencePoint> references_;
  std::vector<TrialNote> reference_notes_;
  double anchor_ = 0.0;
  std::size_t pass_ = 0;
  std::size_t since_reference_ = 0;
  std::size_t trials_ = 0;
  std::size_t settle_trials_ = 0;
  std::size_t unsettled_trials_ = 0;
  std::size_t rounds_ = 0;
};

}  // namespace

Search::Search(SearchConfig config, BoundObjective bound) noexcept
    : config_(std::move(config)), bound_(std::move(bound)) {}

core::Result<Search, ExploreError> Search::configure(
    SearchConfig config, const telemetry::CapabilityReport& capabilities) {
  auto bound = bind(config.objective, capabilities);
  if (!bound) {
    return bound.error();
  }
  if (auto problem = parameter_problem(config)) {
    return make_error(ExploreFault::kConfiguration, std::move(*problem));
  }
  if (auto problem = identity_problem(config)) {
    return make_error(ExploreFault::kConfiguration, std::move(*problem));
  }
  for (const Candidate& candidate : config.candidates) {
    if (!(candidate.duty == config.reference.duty)) {
      return make_error(ExploreFault::kDutyCycle, "candidate " + candidate.id +
                                                      " verifies at a different duty cycle than " +
                                                      config.reference.id);
    }
  }
  if (auto problem = settle_problem(config.reference, capabilities)) {
    return *problem;
  }
  for (const Candidate& candidate : config.candidates) {
    if (auto problem = settle_problem(candidate, capabilities)) {
      return *problem;
    }
  }
  return Search(std::move(config), std::move(bound).value());
}

core::Result<SearchReport, ExploreError> Search::run(TrialRunner& runner) const {
  return Execution(config_, bound_, runner).run();
}

core::Result<SearchReport, ExploreError> explore(TrialRunner& runner, SearchConfig config) {
  auto search = Search::configure(std::move(config), runner.capabilities());
  if (!search) {
    return search.error();
  }
  return search.value().run(runner);
}

}  // namespace loadforge::explore
