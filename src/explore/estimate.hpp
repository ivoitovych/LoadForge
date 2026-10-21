// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_EXPLORE_ESTIMATE_HPP
#define LOADFORGE_EXPLORE_ESTIMATE_HPP

// What a set of replicates comes to: a mean, and -- from two replicates on --
// the spread and the confidence interval around it. A worst-case claim is a
// WorstCase, which cannot be built from fewer than two replicates, so a bare
// number is not a thing explore can report (F5, M6 exit criterion).

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace loadforge::explore {

/// The two-sided confidence level every interval here is at.
inline constexpr double kConfidenceLevel = 0.95;

/// What correcting for drift adds to the uncertainty of a mean. Correcting
/// subtracts a baseline made of noisy reference readings, so the corrected
/// mean carries their noise -- shared between replicates, which the sample
/// spread of the replicates cannot see.
///
///   variance     of the mean's error from the reference's noise.
///   max_degrees  the reference readings' own degrees of freedom: the t value
///                is looked up at the smaller of this and n - 1.
struct Correction {
  double variance = 0.0;
  std::size_t max_degrees = std::numeric_limits<std::size_t>::max();
};

/// Replicates of one measurement.
///
///   replicates   how many; 0 only for an empty set.
///   mean         of the replicates, in the order given.
///   stddev       the sample standard deviation (n - 1); absent below 2.
///   half_width   of the 95% confidence interval of the mean:
///                t * sqrt(stddev^2 / n + correction variance), Student's t;
///                absent below 2.
struct Estimate {
  std::size_t replicates = 0;
  double mean = 0.0;
  std::optional<double> stddev;
  std::optional<double> half_width;
};

[[nodiscard]] bool operator==(const Estimate& lhs, const Estimate& rhs) noexcept;

/// The two-sided 95% critical value of Student's t for `degrees` of freedom
/// (at least 1). Between table entries the value for the smaller degrees of
/// freedom is used, which is the larger value: the interval errs wide.
[[nodiscard]] double t_critical(std::size_t degrees) noexcept;

/// The estimate of `samples`, summed left to right (deterministic: the same
/// samples in the same order give the same bits).
[[nodiscard]] Estimate estimate(std::span<const double> samples,
                                const Correction& correction = {}) noexcept;

/// A worst-case claim: always with the replicate count, the spread and the
/// interval.
struct WorstCase {
  std::string candidate;
  std::size_t replicates = 0;  ///< At least 2.
  double mean = 0.0;
  double stddev = 0.0;
  double half_width = 0.0;         ///< Includes the correction's uncertainty.
  double correction_stddev = 0.0;  ///< The reference noise's share, as a standard error.
};

/// The claim for `candidate` from its `samples`; absent below two replicates,
/// because a spread cannot be said of fewer.
[[nodiscard]] std::optional<WorstCase> worst_case_of(const std::string& candidate,
                                                     std::span<const double> samples,
                                                     const Correction& correction = {});

/// "word_sum: 81.2 +/- 1.4 (95% CI, n=5, sd 1.7, correction sd 0.3)": a claim as a human reads it.
[[nodiscard]] std::string format(const WorstCase& claim);

}  // namespace loadforge::explore

#endif
