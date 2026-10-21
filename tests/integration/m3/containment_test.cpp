// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the stack does when a part of it breaks its contract or runs out of
// room: the fault stays where it happened, is said where a reader will see it,
// and never becomes a number (M3 exit criterion 3).

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "machine.hpp"
#include "main/run_result.hpp"
#include "platform/fs.hpp"
#include "reporters/json/json_writer.hpp"
#include "stack.hpp"
#include "statistics/summarizer.hpp"
#include "telemetry/sampler.hpp"
#include "telemetry/timeline/bounded_timeline.hpp"

namespace loadforge::m3 {
namespace {

/// A source that appends one reading too many.
class Greedy final : public telemetry::Source {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "greedy"; }
  std::vector<telemetry::MetricStatus> probe() override {
    return {{telemetry::Metric{"greedy.value", telemetry::Unit::kCount, telemetry::Kind::kGauge,
                               telemetry::Scope::kMachine, 0, std::nullopt, "test"},
             std::nullopt}};
  }
  void sample(std::vector<telemetry::Reading>& out) override {
    out.emplace_back(telemetry::Gauge{1});
    out.emplace_back(telemetry::Gauge{2});
  }
};

TEST(ContainmentTest, ASourceThatBreaksItsContractIsReportedMalformedAndShiftsNothing) {
  Machine machine;
  machine.install_everything();
  platform::FileSystem files(machine.syscalls());
  telemetry::BoundedTimeline timeline(10'000);
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::make_unique<Greedy>());
  for (auto& real : cli::make_sources(machine.syscalls(), files, cli::MachineRoots{})) {
    sources.push_back(std::move(real));
  }
  const auto sampler = telemetry::make_sampler(std::move(sources), timeline);
  ASSERT_TRUE(sampler->probe().has_value());
  std::int64_t now = kSecond;
  for (int round = 0; round < 3; ++round, now += kSecond) {
    ASSERT_TRUE(sampler->round(now).has_value());
  }
  // Greedy's series is Unavailable-malformed, with the source and both counts.
  const auto greedy = timeline.points(0);
  ASSERT_EQ(greedy.size(), 3U);
  const auto* malformed = std::get_if<telemetry::Unavailable>(&greedy[0].reading);
  ASSERT_NE(malformed, nullptr);
  EXPECT_EQ(malformed->kind, topology::Availability::kMalformed);
  EXPECT_NE(malformed->detail.find("greedy"), std::string::npos);
  EXPECT_NE(malformed->detail.find('2'), std::string::npos);
  EXPECT_NE(malformed->detail.find('1'), std::string::npos);
  EXPECT_FALSE(malformed->remediation.empty());
  // And the next source's first series holds its own first reading.
  const auto thermal = timeline.points(1);
  ASSERT_EQ(thermal.size(), 3U);
  EXPECT_EQ(timeline.metric(1).name, "thermal.package");
  const auto* package = std::get_if<telemetry::Gauge>(&thermal[0].reading);
  ASSERT_NE(package, nullptr);
  EXPECT_DOUBLE_EQ(package->value, 45.0);

  // The summary says "malformed", and no number.
  const statistics::CompensatedSummarizer summarizer;
  const auto signature = summarizer.summarize(timeline, {kSecond, now}).value();
  const auto* summary = std::get_if<telemetry::Unavailable>(&signature.metrics[0].value);
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->kind, topology::Availability::kMalformed);
  EXPECT_EQ(signature.metrics[0].unavailable_samples, 3U);
}

TEST(ContainmentTest, ATimelineThatFillsKeepsWhatItHoldsRefusesTheRestAndTheStatisticsStillWork) {
  Stack stack;
  stack.machine.install_everything();
  // Room for the first round of 27 series and a third of the second.
  stack.start(27 + 9);
  stack.round();
  const auto refused = stack.rig->sampler().round(stack.now);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, telemetry::TimelineFault::kCapacity);

  // The points that were appended before the refusal stay (sampler.hpp), so the
  // series are of different lengths, and the statistics take each as it is.
  std::size_t longer = 0;
  for (std::size_t at = 0; at < stack.timeline().series_count(); ++at) {
    longer += stack.timeline().points(static_cast<telemetry::SeriesId>(at)).size() > 1 ? 1 : 0;
  }
  EXPECT_EQ(longer, 9U);
  const auto taken = stack.summarizer.summarize(stack.timeline(), {kSecond, 3 * kSecond});
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken.value().metrics.size(), 27U);
}

}  // namespace
}  // namespace loadforge::m3
