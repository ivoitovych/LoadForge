// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore/estimate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <tuple>

namespace loadforge::explore {

namespace {

// Two-sided 95% critical values of Student's t, one per degree of freedom
// from 1 to 30 (any statistics table; NIST/SEMATECH e-Handbook 1.3.6.7.2).
constexpr std::array<double, 30> kTable = {12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306,
                                           2.262,  2.228, 2.201, 2.179, 2.160, 2.145, 2.131, 2.120,
                                           2.110,  2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064,
                                           2.060,  2.056, 2.052, 2.048, 2.045, 2.042};

struct Step {
  std::size_t from;
  double t;
};

// Beyond 30 the table is sparse; the value at the largest listed degrees not
// above the request is used, which is never smaller than the true one. Past
// 120 the normal limit (1.960) is not used either: 1.980 stays, a hair wide.
constexpr std::array<Step, 3> kSteps = {{{120, 1.980}, {60, 2.000}, {40, 2.021}}};

struct Moments {
  double mean = 0.0;
  double stddev = 0.0;  ///< 0 below two samples; callers know n.
};

Moments moments(std::span<const double> samples) noexcept {
  Moments out;
  if (samples.empty()) {
    return out;
  }
  double sum = 0.0;
  for (const double x : samples) {
    sum += x;
  }
  out.mean = sum / static_cast<double>(samples.size());
  if (samples.size() < 2) {
    return out;
  }
  double squares = 0.0;
  for (const double x : samples) {
    const double d = x - out.mean;
    squares += d * d;
  }
  out.stddev = std::sqrt(squares / static_cast<double>(samples.size() - 1));
  return out;
}

// The degrees of freedom of the sum of two variance estimates, the replicates'
// variance of the mean (n - 1 degrees) and the reference correction's (its own),
// by Welch and Satterthwaite: each counts in proportion to its share of the sum,
// so the result lies between the smaller of the two and their total; it is capped at
// the replicates' n - 1 below. The smaller
// alone let a reference that added almost nothing to the variance set the t value:
// with the reference credited with the few degrees its estimate has (drift.cpp),
// that made the interval about three times too wide and its coverage 99.8% where
// the search claims 95% (the search property test).
std::size_t combined_degrees(double sample_term, std::size_t n,
                             const Correction& correction) noexcept {
  if (correction.variance <= 0.0) {
    return n - 1;
  }
  const auto sample_degrees = static_cast<double>(n - 1);
  const auto reference_degrees = static_cast<double>(correction.max_degrees);
  const double total = sample_term + correction.variance;
  const double spread = (sample_term * sample_term / sample_degrees) +
                        (correction.variance * correction.variance / reference_degrees);
  // At most the replicates' own n - 1: their spread is estimated from that many and no
  // more, so adding a reference never lends the interval a smaller t than the replicates
  // alone would have.
  const double degrees = std::min(sample_degrees, std::floor(total * total / spread));
  return static_cast<std::size_t>(std::max(1.0, degrees));
}

double half_width_of(double stddev, std::size_t n, const Correction& correction) noexcept {
  const double sample_term = stddev * stddev / static_cast<double>(n);
  return t_critical(combined_degrees(sample_term, n, correction)) *
         std::sqrt(sample_term + correction.variance);
}

}  // namespace

bool operator==(const Estimate& lhs, const Estimate& rhs) noexcept {
  return std::tie(lhs.replicates, lhs.mean, lhs.stddev, lhs.half_width) ==
         std::tie(rhs.replicates, rhs.mean, rhs.stddev, rhs.half_width);
}

double t_critical(std::size_t degrees) noexcept {
  if (degrees <= kTable.size()) {
    const std::size_t index = degrees == 0 ? 0 : degrees - 1;
    return *std::next(kTable.begin(), static_cast<std::ptrdiff_t>(index));
  }
  for (const Step& step : kSteps) {
    if (degrees >= step.from) {
      return step.t;
    }
  }
  return kTable.back();
}

Estimate estimate(std::span<const double> samples, const Correction& correction) noexcept {
  Estimate out;
  out.replicates = samples.size();
  const Moments m = moments(samples);
  out.mean = m.mean;
  if (samples.size() >= 2) {
    out.stddev = m.stddev;
    out.half_width = half_width_of(m.stddev, samples.size(), correction);
  }
  return out;
}

std::optional<WorstCase> worst_case_of(const std::string& candidate,
                                       std::span<const double> samples,
                                       const Correction& correction) {
  if (samples.size() < 2) {
    return std::nullopt;
  }
  const Moments m = moments(samples);
  return WorstCase{candidate,
                   samples.size(),
                   m.mean,
                   m.stddev,
                   half_width_of(m.stddev, samples.size(), correction),
                   std::sqrt(correction.variance)};
}

std::string format(const WorstCase& claim) {
  return std::format("{}: {:.6g} +/- {:.6g} (95% CI, n={}, sd {:.6g}, correction sd {:.6g})",
                     claim.candidate, claim.mean, claim.half_width, claim.replicates, claim.stddev,
                     claim.correction_stddev);
}

}  // namespace loadforge::explore
