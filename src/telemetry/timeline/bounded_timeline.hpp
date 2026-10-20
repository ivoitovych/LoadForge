// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TELEMETRY_TIMELINE_BOUNDED_TIMELINE_HPP
#define LOADFORGE_TELEMETRY_TIMELINE_BOUNDED_TIMELINE_HPP

// The provider of telemetry/timeline.hpp: every series in memory, bounded by
// the number of points it was built for, refusing the point after the last
// rather than dropping an old one.
//
// MEMORY
// ------
// A point is sizeof(Point) bytes (the contract's Point carries a variant
// whose largest alternative is an Unavailable with three strings), and a
// series is a contiguous vector of them because points() hands out a span.
// A vector grows geometrically, so the memory a timeline holds is bounded in
// two figures, both stated by memory_bound_bytes():
//
//   resident   at most 2 * capacity * sizeof(Point): a vector's capacity is
//              never more than twice its size.
//   transient  while one series grows, its old buffer and its new one are
//              both live: at most capacity * sizeof(Point) more, because no
//              series holds more than `capacity` points. Only one series
//              grows at a time (one writer).
//
// The bound is therefore 3 * capacity * sizeof(Point). A five-hour run at
// 10 Hz over 64 series is run_capacity(18000, 10, 64) = 11,520,000 points.
// Nothing is reserved up front: a run that ends early holds what it sampled.

#include <cstddef>
#include <vector>

#include "telemetry/timeline.hpp"

namespace loadforge::telemetry {

class BoundedTimeline final : public Timeline {
 public:
  /// Holds at most `capacity` points over all series.
  explicit BoundedTimeline(std::size_t capacity) noexcept : capacity_(capacity) {}

  [[nodiscard]] core::Result<SeriesId, TimelineError> add_series(const Metric& metric) override;
  [[nodiscard]] core::Result<core::Ok, TimelineError> append(SeriesId series,
                                                             const Point& point) override;
  [[nodiscard]] core::Result<core::Ok, TimelineError> mark_phase(PhaseMark mark) override;

  [[nodiscard]] std::size_t series_count() const noexcept override { return series_.size(); }
  [[nodiscard]] const Metric& metric(SeriesId series) const override {
    return series_[series].metric;
  }
  [[nodiscard]] std::span<const Point> points(SeriesId series) const override {
    return series_[series].points;
  }
  [[nodiscard]] std::span<const PhaseMark> phases() const override { return phases_; }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  /// Points held, over all series.
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  /// Points the series' buffers have room for, over all series.
  [[nodiscard]] std::size_t reserved() const noexcept;

 private:
  struct Series {
    Metric metric;
    std::vector<Point> points;
  };

  std::size_t capacity_;
  std::size_t size_ = 0;
  std::vector<Series> series_;
  std::vector<PhaseMark> phases_;
};

/// Points a run holds: `seconds` of it, `hertz` rounds a second, `series`
/// series, saturating at the largest size_t.
[[nodiscard]] std::size_t run_capacity(std::size_t seconds, std::size_t hertz,
                                       std::size_t series) noexcept;

/// The most memory, in bytes, a timeline of `capacity` points holds,
/// transient growth included (see MEMORY above), saturating.
[[nodiscard]] std::size_t memory_bound_bytes(std::size_t capacity) noexcept;

}  // namespace loadforge::telemetry

#endif
