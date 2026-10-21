// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR 0003, rule 3, for the command line, which assembles a run out of the
// pieces the telemetry, statistics and results streams provide: it consumes
// the sampler, the timeline, the sources, the summarizer, the verdict and the
// result writers. Each is mocked here from its contract header alone, and the
// provider's suite is run against the mock.

#include <gtest/gtest.h>

#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "contracts/results/writer_contract.hpp"
#include "contracts/statistics/signature_contract.hpp"
#include "contracts/telemetry/sampler_contract.hpp"
#include "contracts/telemetry/source_contract.hpp"
#include "contracts/telemetry/timeline_contract.hpp"
#include "controller/results/result.hpp"
#include "controller/supervision.hpp"
#include "unit/controller/telemetry_mocks.hpp"
#include "verification/record.hpp"

namespace loadforge::contracts {
namespace {

struct MainMockSampler {
  static std::unique_ptr<telemetry::Sampler> make(
      std::vector<std::unique_ptr<telemetry::Source>> sources, telemetry::TimelineSink& timeline) {
    return std::make_unique<mocks::MockSampler>(std::move(sources), timeline);
  }
};

struct MainMockTimeline {
  static std::unique_ptr<telemetry::Timeline> make(std::size_t capacity) {
    return std::make_unique<mocks::MockTimeline>(capacity);
  }
};

/// A source in the state the suite's fixture would choose: present, absent
/// and vanishing in one object, so every kind of reading is read.
struct MainMockSource {
  static constexpr int kRounds = 3;
  static std::unique_ptr<telemetry::Source> make() {
    telemetry::Metric gauge{"mock.temp",
                            telemetry::Unit::kCelsius,
                            telemetry::Kind::kGauge,
                            telemetry::Scope::kMachine,
                            0,
                            std::nullopt,
                            "mock"};
    telemetry::Metric counter{"mock.events",
                              telemetry::Unit::kCount,
                              telemetry::Kind::kCounter,
                              telemetry::Scope::kMachine,
                              0,
                              std::uint64_t{100},
                              "mock"};
    telemetry::Metric missing{"mock.fan",
                              telemetry::Unit::kRpm,
                              telemetry::Kind::kGauge,
                              telemetry::Scope::kMachine,
                              0,
                              std::nullopt,
                              "mock"};
    std::vector<telemetry::MetricStatus> statuses = {
        {gauge, std::nullopt},
        {counter, std::nullopt},
        {missing, telemetry::Unavailable{topology::Availability::kAbsent, "/sys/x",
                                         "no fan was found", "nothing can be changed"}}};
    std::vector<std::vector<telemetry::Reading>> rounds = {
        {telemetry::Gauge{40.0}, telemetry::Counter{99}},
        {telemetry::Unavailable{topology::Availability::kVanished, "/sys/x", "gone", "re-plug"},
         telemetry::Counter{3}},
        {telemetry::Gauge{41.0}, telemetry::Counter{4}}};
    return std::make_unique<mocks::MockSource>("mock", std::move(statuses), std::move(rounds));
  }
};

struct MainMockSummarizer {
  static std::unique_ptr<statistics::Summarizer> make() {
    return std::make_unique<mocks::MockSummarizer>();
  }
};

/// The four rules of result.hpp, in the order it states them.
struct MainMockVerdict {
  static bool advanced(const results::RunResult& run, std::string_view name) {
    for (const statistics::MetricSummary& summary : run.signature.metrics) {
      const auto* counter = std::get_if<statistics::CounterSummary>(&summary.value);
      if (summary.metric.name == name && counter != nullptr && counter->delta > 0) {
        return true;
      }
    }
    return false;
  }
  static results::Verdict derive(const results::RunResult& run) {
    bool unrun = false;
    bool wrong = false;
    for (const results::WorkloadResult& workload : run.workloads) {
      for (const results::Finding& finding : workload.findings) {
        if (std::holds_alternative<verification::VerificationNotPerformed>(finding.record)) {
          unrun = true;
        } else {
          wrong = true;
        }
      }
    }
    if (wrong || advanced(run, results::kUncorrectedErrorsMetric)) {
      return results::Verdict::kFindings;
    }
    bool unfinished = run.workloads.empty() || unrun;
    for (const results::WorkloadResult& workload : run.workloads) {
      unfinished =
          unfinished || workload.tally.oracle_blocks == 0 || workload.tally.integrity_checked == 0;
    }
    for (const results::WorkerResult& worker : run.workers) {
      unfinished = unfinished || !controller::is_clean(worker.fate);
    }
    if (unfinished) {
      return results::Verdict::kIncomplete;
    }
    return advanced(run, results::kCorrectedErrorsMetric) ? results::Verdict::kCorrectedErrors
                                                          : results::Verdict::kClean;
  }
};

/// A writer for a format of its own: one "key=value" per fact.
class MockWriter final : public reporters::ResultWriter {
 public:
  [[nodiscard]] std::string_view format() const noexcept override { return "mock"; }
  [[nodiscard]] core::Result<core::Ok, reporters::WriteError> write(
      const results::RunResult& run, std::ostream& out) const override {
    if (!out.good()) {
      return reporters::WriteError{"the stream was not good before writing"};
    }
    std::ostringstream text;
    text << "schema_version=" << run.schema_version << "\n"
         << "elapsed_ns=" << run.elapsed_ns << "\n"
         << "verdict=" << results::describe(run.verdict) << "\n";
    for (const telemetry::SourceStatus& source : run.capabilities.sources) {
      for (const telemetry::MetricStatus& status : source.metrics) {
        if (status.unavailable.has_value()) {
          absence(text, status.metric.name, *status.unavailable);
        }
      }
    }
    for (const statistics::MetricSummary& summary : run.signature.metrics) {
      if (const auto* gone = std::get_if<telemetry::Unavailable>(&summary.value)) {
        absence(text, summary.metric.name, *gone);
      }
    }
    for (const results::WorkloadResult& workload : run.workloads) {
      text << "workload=" << workload.descriptor.name << "\n";
      for (const results::Finding& finding : workload.findings) {
        text << "finding=" << verification::kind_name(finding.record) << "\n";
      }
    }
    out << text.str();
    out.flush();
    if (!out.good()) {
      return reporters::WriteError{"the stream refused the output"};
    }
    return core::Ok{};
  }

 private:
  static void absence(std::ostringstream& text, const std::string& name,
                      const telemetry::Unavailable& gone) {
    text << name << ".state=" << topology::describe(gone.kind) << "\n"
         << name << ".path=" << gone.path << "\n"
         << name << ".detail=" << gone.detail << "\n"
         << name << ".remediation=" << gone.remediation << "\n";
  }
};

struct MainMockWriter {
  static constexpr const char* kFormat = "mock";
  static std::unique_ptr<reporters::ResultWriter> make() { return std::make_unique<MockWriter>(); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(MainMock, SamplerContract, ::testing::Types<MainMockSampler>);
INSTANTIATE_TYPED_TEST_SUITE_P(MainMock, TimelineContract, ::testing::Types<MainMockTimeline>);
INSTANTIATE_TYPED_TEST_SUITE_P(MainMock, SourceContract, ::testing::Types<MainMockSource>);
INSTANTIATE_TYPED_TEST_SUITE_P(MainMock, SummarizerContract, ::testing::Types<MainMockSummarizer>);
INSTANTIATE_TYPED_TEST_SUITE_P(MainMock, VerdictContract, ::testing::Types<MainMockVerdict>);
INSTANTIATE_TYPED_TEST_SUITE_P(MainMock, ResultWriterContract, ::testing::Types<MainMockWriter>);

}  // namespace
}  // namespace loadforge::contracts
