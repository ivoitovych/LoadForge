// SPDX-License-Identifier: GPL-3.0-or-later
//
// The trial runner against the real thing (ADR 0003, the real-against-real
// tier): real worker processes started by the real controller, this machine's
// own /proc and /sys, and the provider's contract suite run on it.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "contracts/explore/trial_contract.hpp"
#include "main/trial_runner.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/snapshot.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::m6 {
namespace {

using cli::ControllerTrialRunner;
using explore::Candidate;

topology::Snapshot describe_machine(platform::Syscalls& syscalls) {
  platform::FileSystem files(syscalls);
  auto taken = topology::take_snapshot(files, topology::kDefaultSysfsRoot);
  EXPECT_TRUE(taken.has_value());
  return std::move(taken).value();
}

/// The runner with everything it stands on.
class RealRunner final : public explore::TrialRunner {
 public:
  RealRunner() : snapshot_(describe_machine(syscalls_)) {
    cli::TrialOptions options;
    options.executable = LOADFORGE_BINARY;
    options.footprint_bytes = std::uint64_t{8} << 20U;
    auto made = ControllerTrialRunner::create(syscalls_, snapshot_, registry_, options);
    EXPECT_TRUE(made.has_value()) << (made.has_value() ? "" : made.error().detail);
    runner_ = std::move(made).value();
  }
  [[nodiscard]] const telemetry::CapabilityReport& capabilities() const override {
    return runner_->capabilities();
  }
  core::Result<explore::TrialOutcome, explore::TrialError> run(
      const Candidate& candidate) override {
    return runner_->run(candidate);
  }

 private:
  platform::RealSyscalls syscalls_;
  topology::Snapshot snapshot_;
  workloads::BuiltinRegistry registry_;
  std::unique_ptr<ControllerTrialRunner> runner_;
};

struct RealControllerRunner {
  // Memory is a gauge every Linux machine has, VM or not.
  static constexpr const char* kPresentGauge = "memory.available";
  static std::unique_ptr<explore::TrialRunner> make() { return std::make_unique<RealRunner>(); }
  static Candidate candidate() {
    return Candidate{"contract", {"sort"}, 1, {}, 3'000'000'000, std::nullopt, {}};
  }
};

}  // namespace
}  // namespace loadforge::m6

namespace loadforge::contracts {
INSTANTIATE_TYPED_TEST_SUITE_P(RealController, TrialRunnerContract,
                               ::testing::Types<m6::RealControllerRunner>);
}  // namespace loadforge::contracts

namespace loadforge::m6 {
namespace {

TEST(M6Runner, ACleanTrialOfAVerifiedWorkloadIsCleanAndMeasuresItsLoad) {
  RealRunner runner;
  Candidate candidate = RealControllerRunner::candidate();
  candidate.threads = 2;
  const auto outcome = runner.run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  EXPECT_EQ(outcome.value().verdict, results::Verdict::kClean);
  // The memory the load held is in the window: the gauge has readings there.
  bool read = false;
  for (const statistics::MetricSummary& summary : outcome.value().signature.metrics) {
    if (summary.metric.name == "memory.available") {
      const auto* gauge = std::get_if<statistics::GaugeSummary>(&summary.value);
      ASSERT_NE(gauge, nullptr);
      EXPECT_GE(gauge->samples, 2U);
      read = true;
    }
  }
  EXPECT_TRUE(read);
}

TEST(M6Runner, ASettlePhaseOnAGaugeThatHoldsStillSettlesBeforeTheLoadStarts) {
  RealRunner runner;
  Candidate candidate = RealControllerRunner::candidate();
  candidate.duration_ns = 1'500'000'000;
  // available memory is not still to the byte; a generous bound over a short window.
  candidate.settle = explore::Settle{"memory.available", 1.0e12, 1'000'000'000, 20'000'000'000};
  const auto outcome = runner.run(candidate);
  ASSERT_TRUE(outcome.has_value()) << outcome.error().detail;
  EXPECT_TRUE(outcome.value().settled);
  EXPECT_GE(outcome.value().settle_ns, 1'000'000'000);
  EXPECT_LT(outcome.value().settle_ns, 20'000'000'000);
}

TEST(M6Runner, ASettleOnTheTemperatureOfAMachineWithNoSensorIsRefusedAndNothingStarts) {
  RealRunner runner;
  bool has_sensor = false;
  for (const auto& source : runner.capabilities().sources) {
    for (const auto& status : source.metrics) {
      has_sensor = has_sensor ||
                   (status.metric.name == "thermal.package" && !status.unavailable.has_value());
    }
  }
  if (has_sensor) {
    GTEST_SKIP() << "this machine has a temperature sensor, which is the other case";
  }
  Candidate candidate = RealControllerRunner::candidate();
  candidate.settle = explore::Settle{"thermal.package"};
  const auto outcome = runner.run(candidate);
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().fault, explore::TrialFault::kMetricUnavailable);
  EXPECT_NE(outcome.error().detail.find("hwmon"), std::string::npos) << outcome.error().detail;
}

}  // namespace
}  // namespace loadforge::m6
