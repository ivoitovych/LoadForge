// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_TELEMETRY_SOURCE_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_TELEMETRY_SOURCE_CONTRACT_HPP

// Provider-side suite for src/telemetry/source.hpp (ADR 0003, 0004).
//
// SourceContract<Subject>:
//   Subject::make()   std::unique_ptr<Source>, built over whatever fixture the
//                     subject chooses. A provider instantiates the suite once
//                     per fixture state -- present, absent, denied, malformed,
//                     vanished, wrapped -- because each must keep the
//                     contract; the hostile-fixture assertions about WHICH
//                     state was reported are the provider's own tests.
//   Subject::kRounds  how many times to sample (at least 1).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "telemetry/timeline.hpp"

namespace loadforge::contracts {

template <typename Subject>
class SourceContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(SourceContract);

using telemetry::Counter;
using telemetry::Gauge;
using telemetry::Kind;
using telemetry::Metric;
using telemetry::MetricStatus;
using telemetry::Reading;
using telemetry::Unavailable;

TYPED_TEST_P(SourceContract, NothingIsSampledBeforeTheProbe) {
  const auto source = TypeParam::make();
  std::vector<Reading> readings;
  source->sample(readings);
  EXPECT_TRUE(readings.empty());
}

TYPED_TEST_P(SourceContract, TheProbeIsNeverSilentAndEveryAbsenceCarriesARemedy) {
  const auto source = TypeParam::make();
  const std::vector<MetricStatus> statuses = source->probe();
  ASSERT_FALSE(statuses.empty()) << "a silently empty column is the worst outcome (F3)";
  const std::string prefix = std::string(source->name()) + ".";
  std::vector<std::tuple<std::string, telemetry::Scope, std::uint32_t>> seen;
  for (const MetricStatus& status : statuses) {
    EXPECT_TRUE(telemetry::is_valid_metric_name(status.metric.name)) << status.metric.name;
    EXPECT_EQ(status.metric.name.rfind(prefix, 0), 0U) << status.metric.name;
    EXPECT_FALSE(status.metric.provider.empty()) << status.metric.name;
    if (status.metric.kind == Kind::kGauge) {
      EXPECT_FALSE(status.metric.modulus.has_value()) << "a gauge does not wrap";
    }
    if (status.unavailable.has_value()) {
      EXPECT_NE(status.unavailable->kind, topology::Availability::kPresent);
      EXPECT_FALSE(status.unavailable->remediation.empty()) << status.metric.name;
      EXPECT_FALSE(status.unavailable->detail.empty()) << status.metric.name;
    }
    const auto identity =
        std::make_tuple(status.metric.name, status.metric.scope, status.metric.scope_index);
    EXPECT_EQ(std::count(seen.begin(), seen.end(), identity), 0) << "two statuses, one series";
    seen.push_back(identity);
  }
}

TYPED_TEST_P(SourceContract, EachRoundReadsEachPresentMetricOnceInProbeOrder) {
  const auto source = TypeParam::make();
  std::vector<Metric> present;
  for (const MetricStatus& status : source->probe()) {
    if (!status.unavailable.has_value()) {
      present.push_back(status.metric);
    }
  }
  for (int round = 0; round < TypeParam::kRounds; ++round) {
    std::vector<Reading> readings = {Gauge{-1.0}};  // sample() appends; it does not clear
    source->sample(readings);
    ASSERT_EQ(readings.size(), present.size() + 1) << "round " << round;
    for (std::size_t at = 0; at < present.size(); ++at) {
      const Reading& reading = readings[at + 1];
      const Metric& metric = present[at];
      if (const auto* gauge = std::get_if<Gauge>(&reading)) {
        EXPECT_EQ(metric.kind, Kind::kGauge) << metric.name;
        EXPECT_TRUE(std::isfinite(gauge->value)) << metric.name;
      } else if (const auto* counter = std::get_if<Counter>(&reading)) {
        EXPECT_EQ(metric.kind, Kind::kCounter) << metric.name;
        if (metric.modulus.has_value()) {
          EXPECT_LT(counter->raw, *metric.modulus) << metric.name;
        }
      } else {
        const auto& unavailable = std::get<Unavailable>(reading);
        EXPECT_NE(unavailable.kind, topology::Availability::kPresent) << metric.name;
        EXPECT_NE(unavailable.kind, topology::Availability::kAbsent)
            << metric.name << ": present at the probe, so gone now, not absent";
        EXPECT_FALSE(unavailable.remediation.empty()) << metric.name;
      }
    }
  }
}

REGISTER_TYPED_TEST_SUITE_P(SourceContract, NothingIsSampledBeforeTheProbe,
                            TheProbeIsNeverSilentAndEveryAbsenceCarriesARemedy,
                            EachRoundReadsEachPresentMetricOnceInProbeOrder);

}  // namespace loadforge::contracts

#endif
