// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_EXPLORE_TRIAL_HPP
#define LOADFORGE_EXPLORE_TRIAL_HPP

// CONTRACT (ADR 0004). What explore needs from the layers below it: a way to
// run one candidate configuration under controlled conditions and get its
// Load Signature back (PLAN F5, IMPLEMENTATION M6). Frozen: changed only by a
// contract change, never inside a stream.
//
//   Provider: the explore-integration stream, over the controller.
//   Consumer: the explore stream (src/explore/): search, objectives, drift
//     correction against an interleaved reference, confidence.
//
// Explore's own rules -- successive halving, replicates and variance, an
// objective whose metric is unavailable failing at configuration time -- are
// explore's, built on this; the runner's part is to make each trial a
// controlled experiment (PLAN §4.4: a controlled starting state, cool-down,
// fixed affinity) and to say when it could not.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "controller/results/result.hpp"
#include "core/result.hpp"
#include "statistics/signature.hpp"
#include "telemetry/source.hpp"
#include "verification/record.hpp"

namespace loadforge::explore {

/// The settle phase before a trial's load (F5, F6): idle until the metric's
/// least-squares slope over the last window_ns is within max_slope_per_second
/// in magnitude, or until max_ns has passed.
struct Settle {
  std::string metric;  ///< A gauge's name: "thermal.package".
  double max_slope_per_second = 0.05;
  std::int64_t window_ns = 30'000'000'000;
  std::int64_t max_ns = 300'000'000'000;
};

[[nodiscard]] inline bool operator==(const Settle& lhs, const Settle& rhs) noexcept {
  return std::tie(lhs.metric, lhs.max_slope_per_second, lhs.window_ns, lhs.max_ns) ==
         std::tie(rhs.metric, rhs.max_slope_per_second, rhs.window_ns, rhs.max_ns);
}

/// One point of the search space.
///
///   id          the candidate's name, for the report; unique within a search.
///   workloads   workload names, at least one, run concurrently.
///   threads     worker processes; at least 1.
///   cpus        where they run; empty means wherever the cpuset allows.
///   duration_ns the measured load, after the settle phase; above 0.
///   settle      the settle phase; absent for none.
///   duty        verification duty cycle, held constant across a search so it
///               is never a confounding variable (F17).
struct Candidate {
  std::string id;
  std::vector<std::string> workloads;
  std::uint32_t threads = 1;
  std::vector<std::uint32_t> cpus;
  std::int64_t duration_ns = 0;
  std::optional<Settle> settle;
  verification::DutyCycle duty;
};

/// What one trial produced.
///
///   candidate    the candidate's id.
///   signature    the Load Signature over exactly the load window: its window
///                is [load start, load start + duration_ns).
///   settled      whether the settle criterion was met (false without one).
///   settle_ns    how long the settle phase took; 0 without one.
///   verdict      the trial's own verdict (results::derive_verdict): a
///                configuration that produced findings is a finding, not a
///                score.
struct TrialOutcome {
  std::string candidate;
  statistics::LoadSignature signature;
  bool settled = false;
  std::int64_t settle_ns = 0;
  results::Verdict verdict = results::Verdict::kIncomplete;
};

/// Why a trial was not run.
///
///   kCandidate          the candidate is not runnable: no workload, an
///                       unknown one, no threads, a CPU the cpuset withholds,
///                       a duration that is not positive.
///   kMetricUnavailable  the settle metric is not present in capabilities():
///                       a settle phase that cannot be measured is refused,
///                       never skipped silently.
///   kRun                the run itself failed: a worker could not start, the
///                       controller could not proceed. The detail says what.
enum class TrialFault : std::uint8_t { kCandidate, kMetricUnavailable, kRun };

[[nodiscard]] constexpr std::string_view describe(TrialFault fault) noexcept {
  switch (fault) {
    case TrialFault::kCandidate:
      return "candidate not runnable";
    case TrialFault::kMetricUnavailable:
      return "settle metric unavailable";
    case TrialFault::kRun:
      return "trial failed to run";
  }
  return "unrecognised trial fault";
}

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct TrialError {
  TrialFault fault = TrialFault::kRun;
  std::string detail;
};

/// Runs trials, one at a time.
///
/// capabilities() is the capability report of the machine the trials run on,
/// probed once; the same answer on every call. Explore binds its objectives
/// against it before the first trial.
///
/// run(candidate) settles, runs the load for duration_ns, and answers the
/// outcome, or the error that kept it from running. Two runs of one
/// candidate are two replicates; nothing is cached between them.
class TrialRunner {
 public:
  TrialRunner() = default;
  TrialRunner(const TrialRunner&) = delete;
  TrialRunner(TrialRunner&&) = delete;
  TrialRunner& operator=(const TrialRunner&) = delete;
  TrialRunner& operator=(TrialRunner&&) = delete;
  virtual ~TrialRunner() = default;

  [[nodiscard]] virtual const telemetry::CapabilityReport& capabilities() const = 0;
  [[nodiscard]] virtual core::Result<TrialOutcome, TrialError> run(const Candidate& candidate) = 0;
};

}  // namespace loadforge::explore

#endif
