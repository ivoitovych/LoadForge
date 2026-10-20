// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4: the timeline against a model, over random appends. Every operation is
// predicted from a model that is written independently of the timeline (a
// map of vectors and the refusal rules from the contract's prose), and after
// each one the timeline must agree on the answer and on everything it holds:
// series are independent in time, each series is in strictly increasing
// order, a refusal changes nothing, and the capacity is never passed.
// The generator is std::mt19937_64 with fixed seeds, so a failure reproduces.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "telemetry/timeline/bounded_timeline.hpp"

namespace loadforge::telemetry {
namespace {

struct ModelSeries {
  Metric metric;
  std::vector<Point> points;
};

Metric metric_for(std::uint32_t index, bool counter) {
  return Metric{"thermal.sensor",
                Unit::kCelsius,
                counter ? Kind::kCounter : Kind::kGauge,
                Scope::kPackage,
                index,
                std::nullopt,
                "test"};
}

class Model {
 public:
  explicit Model(std::size_t capacity) : capacity_(capacity) {}

  std::optional<TimelineFault> add(const Metric& metric) {
    for (const ModelSeries& existing : series_) {
      if (same_series(existing.metric, metric)) {
        return TimelineFault::kDuplicateSeries;
      }
    }
    series_.push_back({metric, {}});
    return std::nullopt;
  }

  std::optional<TimelineFault> append(SeriesId id, const Point& point) {
    if (id >= series_.size()) {
      return TimelineFault::kUnknownSeries;
    }
    ModelSeries& s = series_[id];
    if (!s.points.empty() && point.timestamp_ns <= s.points.back().timestamp_ns) {
      return TimelineFault::kOutOfOrder;
    }
    const bool gauge = std::holds_alternative<Gauge>(point.reading);
    const bool counter = std::holds_alternative<Counter>(point.reading);
    if ((gauge && s.metric.kind != Kind::kGauge) || (counter && s.metric.kind != Kind::kCounter)) {
      return TimelineFault::kKindMismatch;
    }
    if (total_ >= capacity_) {
      return TimelineFault::kCapacity;
    }
    s.points.push_back(point);
    ++total_;
    return std::nullopt;
  }

  [[nodiscard]] const std::vector<ModelSeries>& series() const { return series_; }
  [[nodiscard]] std::size_t total() const { return total_; }

 private:
  std::size_t capacity_;
  std::size_t total_ = 0;
  std::vector<ModelSeries> series_;
};

Reading random_reading(std::mt19937_64& rng) {
  switch (rng() % 3) {
    case 0:
      return Gauge{static_cast<double>(rng() % 1000)};
    case 1:
      return Counter{rng() % 1000};
    default:
      return Unavailable{topology::Availability::kVanished, "/sys/x", "gone", "re-plug"};
  }
}

void expect_same(const BoundedTimeline& timeline, const Model& model) {
  ASSERT_EQ(timeline.series_count(), model.series().size());
  ASSERT_EQ(timeline.size(), model.total());
  std::size_t counted = 0;
  for (SeriesId id = 0; id < timeline.series_count(); ++id) {
    const auto points = timeline.points(id);
    const auto& expected = model.series()[id].points;
    ASSERT_EQ(points.size(), expected.size());
    for (std::size_t at = 0; at < points.size(); ++at) {
      ASSERT_EQ(points[at], expected[at]);
      if (at > 0) {
        ASSERT_LT(points[at - 1].timestamp_ns, points[at].timestamp_ns);
      }
    }
    counted += points.size();
  }
  ASSERT_EQ(counted, timeline.size());
  ASSERT_LE(timeline.size(), timeline.capacity());
  ASSERT_LE(timeline.reserved(), 2 * timeline.size());
}

TEST(TimelineProperty, RandomOperationsAgreeWithTheModelAndKeepTheInvariants) {
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    std::mt19937_64 rng(seed);
    const std::size_t capacity = rng() % 120;
    BoundedTimeline timeline(capacity);
    Model model(capacity);
    for (int step = 0; step < 400; ++step) {
      if (rng() % 10 == 0) {
        const Metric metric = metric_for(static_cast<std::uint32_t>(rng() % 6), rng() % 2 == 0);
        const auto expected = model.add(metric);
        const auto got = timeline.add_series(metric);
        ASSERT_EQ(got.has_value(), !expected.has_value()) << "seed " << seed;
        if (expected.has_value()) {
          ASSERT_EQ(got.error().fault, *expected);
        } else {
          ASSERT_EQ(got.value(), model.series().size() - 1);
        }
      } else {
        const auto id = static_cast<SeriesId>(rng() % 8);  // sometimes unknown
        const Point point{static_cast<std::int64_t>(rng() % 60), random_reading(rng)};
        const auto expected = model.append(id, point);
        const auto got = timeline.append(id, point);
        ASSERT_EQ(got.has_value(), !expected.has_value()) << "seed " << seed << " step " << step;
        if (expected.has_value()) {
          ASSERT_EQ(got.error().fault, *expected);
        }
      }
      expect_same(timeline, model);
      if (::testing::Test::HasFatalFailure()) {
        return;
      }
    }
  }
}

TEST(TimelineProperty, PhasesStayStrictlyIncreasingWhateverIsOffered) {
  for (std::uint64_t seed = 1; seed <= 50; ++seed) {
    std::mt19937_64 rng(seed);
    BoundedTimeline timeline(0);
    std::vector<PhaseMark> accepted;
    for (int step = 0; step < 100; ++step) {
      const PhaseMark mark{static_cast<std::int64_t>(rng() % 50), "p" + std::to_string(step)};
      const bool later = accepted.empty() || mark.start_ns > accepted.back().start_ns;
      const auto got = timeline.mark_phase(mark);
      ASSERT_EQ(got.has_value(), later);
      if (later) {
        accepted.push_back(mark);
      } else {
        ASSERT_EQ(got.error().fault, TimelineFault::kOutOfOrder);
      }
    }
    const auto held = timeline.phases();
    ASSERT_EQ(held.size(), accepted.size());
    for (std::size_t at = 0; at < held.size(); ++at) {
      ASSERT_EQ(held[at], accepted[at]);
    }
  }
}

}  // namespace
}  // namespace loadforge::telemetry
