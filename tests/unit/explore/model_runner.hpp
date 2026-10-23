// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_EXPLORE_MODEL_RUNNER_HPP
#define LOADFORGE_TESTS_UNIT_EXPLORE_MODEL_RUNNER_HPP

// This stream's own reading of TrialRunner (ADR 0003): a runner that runs
// nothing, advances a timeline by each trial's settle and load, and answers a
// signature whose "thermal.package" mean is whatever the test's model says it
// is at that moment. It is checked against TrialRunnerContract in
// runner_contract_test.cpp, so a reading the contract does not hold is caught
// there rather than here.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "explore/search.hpp"
#include "explore/trial.hpp"

namespace loadforge::explore_test {

using explore::Candidate;
using explore::TrialError;
using explore::TrialFault;
using explore::TrialOutcome;

/// How the objective's metric appears in the signature.
enum class Shape : std::uint8_t { kGauge, kSingleReading, kUnavailable, kMissing, kCounter };

inline telemetry::Metric thermal_metric(std::uint32_t index = 0) {
  return telemetry::Metric{"thermal.package",
                           telemetry::Unit::kCelsius,
                           telemetry::Kind::kGauge,
                           telemetry::Scope::kPackage,
                           index,
                           std::nullopt,
                           "hwmon:coretemp"};
}

inline telemetry::Metric energy_metric() {
  return telemetry::Metric{"power.energy",
                           telemetry::Unit::kJoules,
                           telemetry::Kind::kCounter,
                           telemetry::Scope::kPackage,
                           0,
                           1ULL << 32,
                           "powercap:intel-rapl"};
}

inline telemetry::Unavailable gone(topology::Availability kind = topology::Availability::kAbsent) {
  return telemetry::Unavailable{kind, "/sys/class/hwmon", "no such sensor",
                                "load the coretemp module"};
}

inline telemetry::CapabilityReport model_capabilities() {
  telemetry::CapabilityReport report;
  report.sources.push_back(
      {"thermal",
       {{thermal_metric(0), std::nullopt},
        {thermal_metric(1), std::nullopt},
        {telemetry::Metric{"thermal.vrm", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                           telemetry::Scope::kDevice, 0, std::nullopt, "hwmon:vrm"},
         gone()}}});
  report.sources.push_back({"power", {{energy_metric(), std::nullopt}}});
  return report;
}

class ModelRunner final : public explore::TrialRunner {
 public:
  using Model = std::function<double(const Candidate&, std::int64_t mid_ns, std::size_t call)>;

  explicit ModelRunner(Model model)
      : model_(std::move(model)), capabilities_(model_capabilities()) {}

  [[nodiscard]] const telemetry::CapabilityReport& capabilities() const override {
    return capabilities_;
  }

  core::Result<TrialOutcome, TrialError> run(const Candidate& candidate) override {
    const std::size_t call = ran.size();
    ran.push_back(candidate.id);
    if (candidate.workloads.empty() || candidate.threads == 0 || candidate.duration_ns <= 0) {
      return TrialError{TrialFault::kCandidate, "not runnable"};
    }
    for (const std::string& workload : candidate.workloads) {
      if (workload != "word_sum") {
        return TrialError{TrialFault::kCandidate, "unknown workload " + workload};
      }
    }
    TrialOutcome outcome;
    outcome.candidate = candidate.id;
    if (candidate.settle.has_value()) {
      if (candidate.settle->metric != "thermal.package") {
        return TrialError{TrialFault::kMetricUnavailable, candidate.settle->metric};
      }
      outcome.settled = !settles || settles(candidate, call);
      outcome.settle_ns = outcome.settled ? candidate.settle->window_ns : candidate.settle->max_ns;
    }
    if (fault) {
      if (auto error = fault(candidate, call)) {
        return *error;
      }
    }
    const std::int64_t start = clock_ + outcome.settle_ns;
    outcome.signature.window = {start, start + candidate.duration_ns};
    clock_ = outcome.signature.window.end_ns;
    outcome.verdict = verdict ? verdict(candidate, call) : results::Verdict::kClean;
    const double value = model_(candidate, start + candidate.duration_ns / 2, call);
    fill(outcome, shape_of(candidate), value);
    return outcome;
  }

  std::function<std::optional<TrialError>(const Candidate&, std::size_t)> fault;
  std::function<results::Verdict(const Candidate&, std::size_t)> verdict;
  /// Whether the settle phase of a trial meets its criterion; absent means it does.
  std::function<bool(const Candidate&, std::size_t)> settles;
  std::function<Shape(const Candidate&)> shape_of = [](const Candidate&) { return Shape::kGauge; };
  std::vector<std::string> ran;

 private:
  static void fill(TrialOutcome& outcome, Shape shape, double value) {
    using statistics::CounterSummary;
    using statistics::GaugeSummary;
    switch (shape) {
      case Shape::kGauge:
        outcome.signature.metrics.push_back(
            {thermal_metric(0),
             GaugeSummary{10, value - 1, value + 1, value, 0.5, value, value, 0.1}, 0});
        break;
      case Shape::kSingleReading:
        outcome.signature.metrics.push_back(
            {thermal_metric(0),
             GaugeSummary{1, value, value, value, std::nullopt, value, value, std::nullopt}, 0});
        break;
      case Shape::kUnavailable:
        outcome.signature.metrics.push_back(
            {thermal_metric(0), gone(topology::Availability::kVanished), 3});
        break;
      case Shape::kMissing:
        break;
      case Shape::kCounter:
        outcome.signature.metrics.push_back(
            {energy_metric(), CounterSummary{5, static_cast<std::uint64_t>(value), value / 2}, 0});
        break;
    }
  }

  Model model_;
  telemetry::CapabilityReport capabilities_;
  std::int64_t clock_ = 1'000;
};

inline Candidate make_candidate(std::string id) {
  return Candidate{std::move(id), {"word_sum"}, 2, {}, 1'000'000, std::nullopt, {}};
}

}  // namespace loadforge::explore_test

#endif
