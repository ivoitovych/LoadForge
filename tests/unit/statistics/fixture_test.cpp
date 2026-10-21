// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1/T3: a Load Signature from a recorded fixture alone. The fixture is a
// real recording of this kernel's /proc (command and machine in its header);
// the expected figures were computed from its numbers in exact rational
// arithmetic (python3 fractions), not by the code under test.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "contracts/statistics/signature_contract.hpp"
#include "statistics/summarizer.hpp"

namespace loadforge::statistics {
namespace {

using contracts::signature_suite::Series;
using telemetry::Counter;
using telemetry::Gauge;
using telemetry::Point;

struct Recording {
  std::vector<Point> gauge;
  std::vector<Point> counter;
};

Recording load() {
  std::ifstream file(std::string(LOADFORGE_SOURCE_DIR) +
                     "/tests/fixtures/timeline/statistics/proc-sample.csv");
  EXPECT_TRUE(file.is_open());
  Recording recording;
  std::string line;
  bool header_seen = false;
  while (std::getline(file, line)) {
    if (line.starts_with('#')) {
      continue;
    }
    if (!header_seen) {
      header_seen = true;
      continue;
    }
    std::istringstream fields(line);
    std::string time;
    std::string counter;
    std::string gauge;
    std::getline(fields, time, ',');
    std::getline(fields, counter, ',');
    std::getline(fields, gauge, ',');
    recording.counter.push_back(Point{std::stoll(time), Counter{std::stoull(counter)}});
    recording.gauge.push_back(Point{std::stoll(time), Gauge{std::stod(gauge)}});
  }
  return recording;
}

TEST(SignatureFromFixture, TheWholeRecordingSummarisesToTheExactRationalFigures) {
  const Recording recording = load();
  ASSERT_EQ(recording.gauge.size(), 12U);
  Series view;
  view.add({"memory.available", telemetry::Unit::kBytes, telemetry::Kind::kGauge,
            telemetry::Scope::kMachine, 0, std::nullopt, "proc:meminfo"},
           recording.gauge);
  view.add({"scheduler.context_switches", telemetry::Unit::kCount, telemetry::Kind::kCounter,
            telemetry::Scope::kMachine, 0, std::nullopt, "proc:stat"},
           recording.counter);
  const auto signature = CompensatedSummarizer().summarize(view, {0, 1'000'000'000});
  ASSERT_TRUE(signature.has_value());
  ASSERT_EQ(signature.value().metrics.size(), 2U);

  const auto& gauge = std::get<GaugeSummary>(signature.value().metrics[0].value);
  EXPECT_EQ(gauge.samples, 12U);
  EXPECT_EQ(gauge.min, 16008488.0);
  EXPECT_EQ(gauge.max, 16009292.0);
  EXPECT_EQ(gauge.first, 16008488.0);
  EXPECT_EQ(gauge.last, 16009292.0);
  EXPECT_NEAR(gauge.mean, 16008993.666666666, 1e-8);
  EXPECT_NEAR(*gauge.stddev, 329.71374177481823, 1e-9);
  EXPECT_NEAR(*gauge.slope_per_second, 1680.5116615776121, 1e-8);

  const auto& counter = std::get<CounterSummary>(signature.value().metrics[1].value);
  EXPECT_EQ(counter.samples, 12U);
  EXPECT_EQ(counter.delta, 209U);
  EXPECT_NEAR(*counter.rate_per_second, 376.47765724172086, 1e-9);
}

}  // namespace
}  // namespace loadforge::statistics
