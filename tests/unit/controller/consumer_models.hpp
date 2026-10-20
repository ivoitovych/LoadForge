// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_CONTROLLER_CONSUMER_MODELS_HPP
#define LOADFORGE_TESTS_UNIT_CONTROLLER_CONSUMER_MODELS_HPP

// The controller's and the command line's own models of the contracts they
// consume, each written from the contract's words (ADR 0003): a telemetry
// source, the floating-point control registers, the ISA probe and the path
// selection, and the isolation ladder. The providers' suites are run against
// them (consumer_models_test.cpp); the controller's tests use them as doubles.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "primitives/simd/dispatch.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/source.hpp"
#include "verification/isolation.hpp"

namespace loadforge::controller_testing {

/// A source of two metrics: a gauge it can read and a counter it cannot, with
/// the remedy the contract asks of every absence.
class ModelSource final : public telemetry::Source {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "model"; }

  std::vector<telemetry::MetricStatus> probe() override {
    probed_ = true;
    const telemetry::Metric temperature{"model.temperature",
                                        telemetry::Unit::kCelsius,
                                        telemetry::Kind::kGauge,
                                        telemetry::Scope::kPackage,
                                        0,
                                        std::nullopt,
                                        "model"};
    const telemetry::Metric energy{"model.energy",
                                   telemetry::Unit::kJoules,
                                   telemetry::Kind::kCounter,
                                   telemetry::Scope::kPackage,
                                   0,
                                   std::nullopt,
                                   "model"};
    telemetry::Unavailable denied{topology::Availability::kDenied, "/model/energy",
                                  "permission denied", "run as a user that may read it"};
    return {telemetry::MetricStatus{temperature, std::nullopt},
            telemetry::MetricStatus{energy, denied}};
  }

  void sample(std::vector<telemetry::Reading>& out) override {
    if (probed_) {
      out.emplace_back(telemetry::Gauge{41.5});
    }
  }

  [[nodiscard]] bool probed() const noexcept { return probed_; }

 private:
  bool probed_ = false;
};

/// The control registers as a pair of values per thread, with the flags of the
/// architecture it models.
template <bool kDaz, bool kDn>
class ModelFp final : public fpenv::FpControl {
 public:
  static constexpr bool kHasDenormalsAreZero = kDaz;
  static constexpr bool kHasDefaultNan = kDn;

  [[nodiscard]] fpenv::FpState read() const override {
    fpenv::FpState state = held();
    if (!kDaz) {
      state.denormals_are_zero.reset();
    }
    if (!kDn) {
      state.default_nan.reset();
    }
    if (kDaz && !state.denormals_are_zero.has_value()) {
      state.denormals_are_zero = false;
    }
    if (kDn && !state.default_nan.has_value()) {
      state.default_nan = false;
    }
    return state;
  }

  void write(const fpenv::FpState& state) override {
    fpenv::FpState& mine = held();
    mine.rounding = state.rounding;
    mine.flush_to_zero = state.flush_to_zero;
    if (kDaz && state.denormals_are_zero.has_value()) {
      mine.denormals_are_zero = state.denormals_are_zero;
    }
    if (kDn && state.default_nan.has_value()) {
      mine.default_nan = state.default_nan;
    }
  }

 private:
  static fpenv::FpState& held() {
    thread_local fpenv::FpState state;
    return state;
  }
};

/// A probe that supports scalar and the paths of the architecture it claims.
class ModelProbe final : public simd::IsaProbe {
 public:
  explicit ModelProbe(simd::IsaSet set) noexcept : set_(set) {}
  [[nodiscard]] simd::IsaSet supported() const override { return set_; }

 private:
  simd::IsaSet set_;
};

/// select_path as the consumer reads dispatch.hpp: a request is used or
/// refused, never replaced; the build's lack before the machine's; scalar
/// never refused; no request, the first of the preference both hold.
struct ModelSelect {
  static core::Result<simd::IsaPath, simd::DispatchError> select(
      std::optional<simd::IsaPath> requested, simd::IsaSet supported, simd::IsaSet compiled) {
    if (requested.has_value()) {
      if (*requested == simd::IsaPath::kScalar) {
        return simd::IsaPath::kScalar;
      }
      if (!compiled.contains(*requested)) {
        return simd::DispatchError{simd::DispatchFault::kNotCompiled, *requested, "not compiled"};
      }
      if (!supported.contains(*requested)) {
        return simd::DispatchError{simd::DispatchFault::kNotSupported, *requested, "not supported"};
      }
      return *requested;
    }
    for (const simd::IsaPath path : simd::kPreference) {
      if (compiled.contains(path) && supported.contains(path)) {
        return path;
      }
    }
    return simd::IsaPath::kScalar;
  }
};

/// The ladder, as verification/isolation.hpp's header reads: same core first;
/// anything but a reproduction stops it; another core when the placement
/// offers one; one thread unless the original ran one.
class ModelIsolator final : public verification::Isolator {
 public:
  verification::IsolationReport isolate(const verification::WrongResult& finding,
                                        const verification::Placement& placement,
                                        verification::Rerunner& rerunner) override {
    using verification::Answer;
    using verification::Rung;
    verification::IsolationReport report;
    const auto ask = [&](Rung rung, std::uint32_t cpu, std::uint32_t threads) {
      const verification::RerunRequest request{rung, cpu, threads};
      const verification::RerunOutcome outcome = rerunner.rerun(finding, request);
      report.steps.push_back(verification::Step{request, outcome});
      switch (outcome) {
        case verification::RerunOutcome::kReproduced:
          return Answer::kYes;
        case verification::RerunOutcome::kClean:
          return Answer::kNo;
        case verification::RerunOutcome::kNotPerformed:
          break;
      }
      return Answer::kUnknown;
    };
    report.answers.same_core = ask(Rung::kSameCore, placement.cpu, placement.threads);
    if (report.answers.same_core == Answer::kYes) {
      if (!placement.other_cores.empty()) {
        report.answers.other_core =
            ask(Rung::kOtherCore, placement.other_cores.front(), placement.threads);
      }
      report.answers.single_thread = placement.threads == 1
                                         ? report.answers.same_core
                                         : ask(Rung::kSingleThread, placement.cpu, 1);
    }
    report.finding = verification::derive_finding(report.answers);
    report.confidence = verification::confidence_of(report.finding);
    return report;
  }
};

}  // namespace loadforge::controller_testing

#endif
