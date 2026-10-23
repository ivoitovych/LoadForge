// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_EXPLORE_DRIFT_HPP
#define LOADFORGE_EXPLORE_DRIFT_HPP

// Drift correction against an interleaved reference (F5). The machine warms
// over a search, so one configuration scores differently at hour 3 than at
// minute 5. A reference candidate is run before the first trial, between
// trials and after the last, and its readings say what the machine was doing
// at each moment; a trial's score is corrected by how far the reference had
// moved by then.
//
// The correction is additive: corrected = raw - (baseline(t) - anchor), where
// baseline(t) is the reference interpolated linearly in time between its two
// neighbouring readings, and anchor is the reference as it was at the start.
// Additive because the drift explore fights is an offset (ambient, a warm
// heatsink, in degrees or watts); a multiplicative form divides by the
// reference and misbehaves near zero.

#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::explore {

/// One reading of the reference candidate: when (the midpoint of its load
/// window, on the timeline's clock) and what the objective scored.
struct ReferencePoint {
  std::int64_t at_ns = 0;
  double value = 0.0;
};

/// The reference at `at_ns`: linear between the neighbouring readings, held
/// at the first before it and the last after it. `points` are in time order
/// (a reading out of order is passed over); empty gives 0.
[[nodiscard]] double baseline_at(std::span<const ReferencePoint> points,
                                 std::int64_t at_ns) noexcept;

/// Where `at_ns` falls among the readings: the baseline there is
/// (1 - fraction) * value[lower] + fraction * value[upper]. `points` must not
/// be empty. Held ends have lower == upper.
struct Bracket {
  std::size_t lower = 0;
  std::size_t upper = 0;
  double fraction = 0.0;
};

[[nodiscard]] Bracket bracket_at(std::span<const ReferencePoint> points,
                                 std::int64_t at_ns) noexcept;

/// The reference's noise, as a variance. Each interior reading is compared
/// with the line through its neighbours in time: a drift that is linear over
/// three readings leaves no residual, so only noise is measured, and the
/// residual's variance is sigma^2 (1 + w^2 + (1 - w)^2) for w the reading's
/// place between the neighbours. With two readings the only evidence is their
/// difference, d^2 / 2, which drift inflates and so errs high. Zero below two.
[[nodiscard]] double reference_variance(std::span<const ReferencePoint> points) noexcept;

/// The degrees of freedom to credit reference_variance() with when `readings`
/// went into it: at least 1, and no more than the effective degrees of freedom
/// of that estimate, so a t interval built on it errs wide. It is
/// max(1, (readings - 2) / 2) in integer arithmetic; drift.cpp has the derivation
/// and the measured values.
[[nodiscard]] std::size_t reference_degrees(std::size_t readings) noexcept;

/// How much of the reference's noise the mean of the corrected scores of
/// trials at `trial_times` carries: the sum of squared weights of the
/// readings in (mean baseline there less the anchor), the anchor being the mean
/// of the first `anchor_count` readings. Multiplied by the noise variance it is
/// the variance of that mean's error. Readings that serve both the baseline
/// and the anchor cancel. `points` must have at least `anchor_count` readings
/// (at least 1); with no trial times the result is the anchor's alone.
[[nodiscard]] double correction_gain(std::span<const ReferencePoint> points,
                                     std::span<const std::int64_t> trial_times,
                                     std::size_t anchor_count);

/// `raw` with the machine's drift since the anchor taken out.
[[nodiscard]] double corrected(double raw, double baseline, double anchor) noexcept;

}  // namespace loadforge::explore

#endif
