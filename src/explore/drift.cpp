// SPDX-License-Identifier: GPL-3.0-or-later
#include "explore/drift.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace loadforge::explore {

Bracket bracket_at(std::span<const ReferencePoint> points, std::int64_t at_ns) noexcept {
  if (at_ns <= points.front().at_ns) {
    return {0, 0, 0.0};
  }
  for (std::size_t i = 1; i < points.size(); ++i) {
    const ReferencePoint& before = points[i - 1];
    const ReferencePoint& after = points[i];
    if (at_ns > after.at_ns) {
      continue;
    }
    // before.at_ns < at_ns <= after.at_ns here, so the span is positive.
    const std::int64_t span_ns = after.at_ns - before.at_ns;
    return {i - 1, i, static_cast<double>(at_ns - before.at_ns) / static_cast<double>(span_ns)};
  }
  return {points.size() - 1, points.size() - 1, 0.0};
}

double baseline_at(std::span<const ReferencePoint> points, std::int64_t at_ns) noexcept {
  if (points.empty()) {
    return 0.0;
  }
  const Bracket where = bracket_at(points, at_ns);
  const double before = points[where.lower].value;
  return before + where.fraction * (points[where.upper].value - before);
}

double reference_variance(std::span<const ReferencePoint> points) noexcept {
  const std::size_t n = points.size();
  if (n < 2) {
    return 0.0;
  }
  if (n == 2) {
    const double step = points[1].value - points[0].value;
    return step * step / 2.0;
  }
  double total = 0.0;
  for (std::size_t i = 1; i + 1 < n; ++i) {
    const auto span_ns = static_cast<double>(points[i + 1].at_ns - points[i - 1].at_ns);
    const double w =
        span_ns > 0.0 ? static_cast<double>(points[i].at_ns - points[i - 1].at_ns) / span_ns : 0.5;
    const double residual =
        points[i].value - (points[i - 1].value + w * (points[i + 1].value - points[i - 1].value));
    total += residual * residual / (1.0 + w * w + (1.0 - w) * (1.0 - w));
  }
  return total / static_cast<double>(n - 2);
}

// The variance reference_variance() returns averages the n - 2 squared, scaled
// residuals of the interior readings, but those residuals are not independent: two
// neighbouring residuals share a reading. Its effective degrees of freedom (the df
// of the chi-square with the same mean and variance, Satterthwaite) is therefore
// below n - 2, and below the n - 1 a sample of n readings would have.
//
// For readings that are equally spaced, with noise of variance s^2 and no
// drift, the residual e_i = x_i - (x_(i-1) + x_(i+1)) / 2 has variance 3/2 s^2,
// correlation -2/3 with its neighbour and 1/6 two places away. For m = n - 2
// residuals, the variance of their mean square is 2 (s^2)^2 / m^2 times
// the sum of their squared correlations, m + 2 (m - 1) 4/9 + 2 (m - 2) 1/36, so
//
//     df_eff = m^2 / (m + 8 (m - 1) / 9 + (m - 2) / 18)   which is  about 0.51 m.
//
// That gives 1.00, 1.38, 1.86, 2.36, 3.38 for 3, 4, 5, 6, 8 readings, and
// simulations of 400000 draws measured 0.997, 1.38, 1.87, 2.34 and 3.38 (the review
// verdicts of 2026-10-23, finding 8; the code had used n - 1:
// 2, 3, 4, 5 and 7, which gave a 95% interval a coverage of 86% for three readings).
// Half of m, rounded down and at least 1, is below that for every n: 1, 1, 1, 2, 2,
// 3 for 3 to 8 readings (two readings give one: d^2 / 2 is a chi-square of 1).
// Unequal spacing changes the correlations a little; the margin absorbs it.
std::size_t reference_degrees(std::size_t readings) noexcept {
  constexpr std::size_t kMostReadingsOnOneDegree = 3;  // (3 - 2) / 2 would be 0
  return readings <= kMostReadingsOnOneDegree ? 1 : (readings - 2) / 2;
}

double correction_gain(std::span<const ReferencePoint> points,
                       std::span<const std::int64_t> trial_times, std::size_t anchor_count) {
  std::vector<double> weight(points.size(), 0.0);
  const double each = 1.0 / static_cast<double>(trial_times.size());
  for (const std::int64_t at_ns : trial_times) {
    const Bracket where = bracket_at(points, at_ns);
    weight[where.lower] += (1.0 - where.fraction) * each;
    weight[where.upper] += where.fraction * each;
  }
  for (std::size_t k = 0; k < anchor_count; ++k) {
    weight[k] -= 1.0 / static_cast<double>(anchor_count);
  }
  double squares = 0.0;
  for (const double w : weight) {
    squares += w * w;
  }
  return squares;
}

double corrected(double raw, double baseline, double anchor) noexcept {
  return raw - (baseline - anchor);
}

}  // namespace loadforge::explore
