// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_EXPLORE_SEARCH_HPP
#define LOADFORGE_EXPLORE_SEARCH_HPP

// Worst-case search by successive halving over a curated candidate set (F5).
//
//   configure   binds the objective against the capability report and checks
//               the search is well formed -- all before the first trial.
//   run         measures the reference (the anchor), then rounds: every
//               surviving candidate is run to the round's replicate count,
//               the reference is interleaved throughout and after the round,
//               drift is taken out of every score, and the worse half (by
//               1/elimination_factor) is kept. The survivor is then run
//               final_replicates more times, replicates that took no part in
//               choosing it, and its mean and interval are from those alone.
//
// What the report promises, and why:
//   * a WorstCase carries replicates >= 2, the sample standard deviation and
//     the 95% interval -- a bare number is not representable;
//   * Ranking says whether the winner can be told from the best rival at that
//     evidence: a constant objective ranks nothing (kIndistinguishable);
//   * a trial that found something is a finding, not a score: that candidate
//     leaves the search with Fate::kFindings and is listed, never ranked;
//   * the replicate order is by pass, every survivor once per pass, so drift
//     meets the candidates equally; the reference brackets every trial.
//
// A scorable signature is required: a trial whose window has no reading of
// the objective ends the search (kNoReading) rather than score zero.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "explore/drift.hpp"
#include "explore/estimate.hpp"
#include "explore/objective.hpp"
#include "explore/trial.hpp"
#include "telemetry/source.hpp"

namespace loadforge::explore {

/// The search as configured.
///
///   objective             what to maximise the badness of.
///   reference             the candidate run repeatedly to measure drift; its
///                         id differs from every candidate's.
///   candidates            the curated set; at least one, ids unique.
///   base_replicates       replicates per candidate in the first round (>= 1);
///                         each later round multiplies it by elimination_factor.
///   elimination_factor    (>= 2) keep ceil(n / factor) candidates per round.
///   final_replicates      fresh replicates of the survivor, run after it has been
///                         chosen (>= 2); its reported mean and interval are from
///                         these alone, because the replicates that chose it are
///                         biased towards the worst (winner's curse). A search
///                         therefore adds final_replicates trials to those of its
///                         rounds, where topping the survivor up to a total
///                         added only what was missing.
///   interleave_every      a reference trial after this many candidate trials
///                         (>= 1).
///   reference_replicates  reference trials before the first candidate; their
///                         mean is the anchor (>= 1).
///
/// Every candidate and the reference share one verification duty cycle, or
/// the search is refused (F17).
struct SearchConfig {
  Objective objective;
  Candidate reference;
  std::vector<Candidate> candidates;
  std::size_t base_replicates = 1;
  std::size_t elimination_factor = 2;
  std::size_t final_replicates = 3;
  std::size_t interleave_every = 4;
  std::size_t reference_replicates = 2;
};

/// What became of a candidate.
///
///   kSurvivor    reached the end of the search.
///   kEliminated  ranked out in a round.
///   kFindings    a trial of it produced findings (verdict kFindings).
///   kIncomplete  a trial of it did not finish (verdict kIncomplete).
enum class Fate : std::uint8_t { kSurvivor, kEliminated, kFindings, kIncomplete };

[[nodiscard]] constexpr std::string_view describe(Fate fate) noexcept {
  switch (fate) {
    case Fate::kSurvivor:
      return "survivor";
    case Fate::kEliminated:
      return "eliminated";
    case Fate::kFindings:
      return "findings";
    case Fate::kIncomplete:
      return "incomplete";
  }
  return "unrecognised fate";
}

/// Whether the worst case can be told from the best rival.
///
///   kNone                 there is no worst case: no candidate survived the
///                         search (findings, an unfinished trial, or the
///                         search ended at the reference).
///   kSole                 no rival: one candidate was searched.
///   kSeparated            the winner's interval and the rival's do not
///                         overlap, in the winner's favour.
///   kIndistinguishable    they overlap, or the rival has too few replicates
///                         for an interval: the evidence does not rank them.
///
/// "The rival" is the eliminated candidate with the worst corrected mean, and
/// only that one: an eliminated candidate with a single replicate has no
/// interval, so it is not compared when another has the worse mean, and
/// SearchReport::uncompared names it. "Separated" is a claim about the rival
/// named there, never about every candidate that left the search.
enum class Ranking : std::uint8_t { kNone, kSole, kSeparated, kIndistinguishable };

[[nodiscard]] constexpr std::string_view describe(Ranking ranking) noexcept {
  switch (ranking) {
    case Ranking::kNone:
      return "no worst case";
    case Ranking::kSole:
      return "sole candidate";
    case Ranking::kSeparated:
      return "separated from the rival with the worst corrected mean";
    case Ranking::kIndistinguishable:
      return "not distinguishable from the rival with the worst corrected mean";
  }
  return "unrecognised ranking";
}

/// What was noted of one scored trial besides its score.
///
///   corrected_errors  the hardware reported an error and corrected it
///                     (verdict kCorrectedErrors). The trial is scored -- its
///                     computation was right, so the number is a measurement --
///                     and flagged, and the search exits 5 as `stress` does.
///   unsettled         the trial asked for a settle phase and it ended at its
///                     maximum without meeting the criterion: the trial did not
///                     start from a controlled state. It is scored (the settle
///                     timeout is documented as no error) and marked.
struct TrialNote {
  bool corrected_errors = false;
  bool unsettled = false;
};

/// One candidate's account. `samples` are the drift-corrected scores, in
/// replicate order, in the metric's unit; `notes` has one entry for each;
/// `raw` and `corrected` are the estimates of the scores before and after
/// correction.
struct CandidateReport {
  std::string id;
  Fate fate = Fate::kEliminated;
  std::size_t rounds = 0;  ///< Rounds the candidate was ranked in.
  std::vector<double> samples;
  std::vector<TrialNote> notes;
  /// The leading replicates of `samples` that took part in choosing this candidate
  /// as the survivor. They are listed, and are not in `raw`, `corrected` or the
  /// worst case, which come from the replicates after them. 0 for a candidate that
  /// was not the survivor.
  std::size_t selection_replicates = 0;
  Estimate raw;
  Estimate corrected;
};

struct SearchReport {
  BoundObjective objective;
  std::vector<CandidateReport> candidates;  ///< In the order configured.
  std::optional<WorstCase> worst_case;
  Ranking ranking = Ranking::kNone;
  std::string rival;  ///< The eliminated candidate the ranking compares with; empty if none.
  /// Eliminated candidates with fewer than two replicates: no interval, so none of
  /// them was compared by one.
  std::vector<std::string> uncompared;
  std::vector<ReferencePoint> references;
  std::vector<TrialNote> reference_notes;  ///< One for each reference reading.
  double anchor = 0.0;                     ///< The reference as it was at the start.
  double drift = 0.0;                      ///< The last reference reading less the anchor.
  Estimate reference;                      ///< All reference readings, raw.
  std::size_t trials = 0;                  ///< Every trial run, reference included.
  /// Scored trials, candidates' and reference's, that had corrected errors.
  std::size_t corrected_error_trials = 0;
  /// Every trial run that asked for a settle phase, scored or not, and how many of
  /// them ended without meeting its criterion.
  std::size_t settle_trials = 0;
  std::size_t unsettled_trials = 0;
  std::size_t rounds = 0;
};

/// A search that has passed configuration.
class Search {
 public:
  /// Binds the objective and checks the search; nothing has run when this
  /// returns, error or not.
  [[nodiscard]] static core::Result<Search, ExploreError> configure(
      SearchConfig config, const telemetry::CapabilityReport& capabilities);

  /// Runs the search on `runner`.
  [[nodiscard]] core::Result<SearchReport, ExploreError> run(TrialRunner& runner) const;

 private:
  Search(SearchConfig config, BoundObjective bound) noexcept;

  SearchConfig config_;
  BoundObjective bound_;
};

/// configure() against the runner's own capability report, then run().
[[nodiscard]] core::Result<SearchReport, ExploreError> explore(TrialRunner& runner,
                                                               SearchConfig config);

}  // namespace loadforge::explore

#endif
