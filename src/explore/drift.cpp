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
