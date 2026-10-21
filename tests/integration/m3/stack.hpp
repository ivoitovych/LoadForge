// SPDX-License-Identifier: GPL-3.0-or-later
//
// The telemetry stack as the command line wires it, driven round by round over
// a Machine: every source, the sampler, the timeline, the statistics, the
// result and both writers -- the products of their own streams, linked as
// they are (ADR 0003, rule 4). The test says what time it is and what the
// machine's files hold; nothing else is scripted.

#ifndef LOADFORGE_TESTS_INTEGRATION_M3_STACK_HPP
#define LOADFORGE_TESTS_INTEGRATION_M3_STACK_HPP

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <variant>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "controller/results/result.hpp"
#include "machine.hpp"
#include "main/run_result.hpp"
#include "main/telemetry_rig.hpp"
#include "platform/fs.hpp"
#include "reporters/csv/csv_writer.hpp"
#include "reporters/json/json_writer.hpp"
#include "statistics/summarizer.hpp"

namespace loadforge::m3 {

constexpr std::int64_t kSecond = 1'000'000'000;

class Stack {
 public:
  Stack() : files_(machine.syscalls()) {}

  /// Probes every source over the machine as it is now.
  void start(std::size_t capacity = 100'000) {
    auto made = cli::make_rig(machine.syscalls(), files_, cli::MachineRoots{}, capacity);
    ASSERT_TRUE(made.has_value()) << made.error().detail;
    rig = std::move(made).value();
    first_ = now;
  }

  /// One round at the current time, then a second later.
  void round() {
    ASSERT_NE(rig, nullptr);
    const auto answer = rig->sampler().round(now);
    ASSERT_TRUE(answer.has_value()) << answer.error().detail;
    last_ = now;
    ++rounds_;
    now += kSecond;
  }

  void rounds(int count) {
    for (int i = 0; i < count; ++i) {
      round();
    }
  }

  [[nodiscard]] const telemetry::TimelineView& timeline() const { return rig->timeline(); }
  [[nodiscard]] const telemetry::CapabilityReport& capabilities() const {
    return rig->capabilities();
  }

  /// The whole run's signature, as the command line takes it.
  [[nodiscard]] statistics::LoadSignature signature() const {
    const controller::SamplingReport sampling = sampling_report();
    const auto taken = summarizer.summarize(
        rig->timeline(), statistics::run_window(sampling.first_round_ns, sampling.last_round_ns));
    EXPECT_TRUE(taken.has_value());
    return taken.value();
  }

  [[nodiscard]] controller::SamplingReport sampling_report() const {
    controller::SamplingReport sampling;
    sampling.enabled = true;
    sampling.interval_ns = kSecond;
    sampling.rounds = rounds_;
    sampling.first_round_ns = first_;
    sampling.last_round_ns = last_;
    return sampling;
  }

  /// The result of a run of two idle workers that completed, as the command
  /// line assembles it.
  [[nodiscard]] results::RunResult result() const {
    config::Config config;
    config.run.mode = config::Mode::kStress;
    config.workloads.push_back(config::Workload{"null", config::Threads::all(), true, {}});
    controller::RunReport report;
    report.elapsed_ns = last_ - first_;
    report.sampling = sampling_report();
    report.workers.push_back(controller::WorkerReport{
        0, 1, controller::WorkerFate::kCompleted, "exited with code 0", "stop requested", 1, {}});
    cli::ResultInputs inputs;
    inputs.config = &config;
    inputs.report = &report;
    inputs.capabilities = rig->capabilities();
    inputs.timeline = &rig->timeline();
    inputs.summarizer = &summarizer;
    auto assembled = cli::assemble_result(inputs);
    EXPECT_TRUE(assembled.has_value());
    return assembled.value();
  }

  [[nodiscard]] static std::string json(const results::RunResult& result) {
    std::ostringstream out;
    EXPECT_TRUE(reporters::JsonWriter{}.write(result, out).has_value());
    return out.str();
  }
  [[nodiscard]] static std::string csv(const results::RunResult& result) {
    std::ostringstream out;
    EXPECT_TRUE(reporters::CsvWriter{}.write(result, out).has_value());
    return out.str();
  }

  /// The summary of the nth series named `name`, from a signature.
  [[nodiscard]] static const statistics::MetricSummary* find(
      const statistics::LoadSignature& signature, const std::string& name, std::size_t nth = 0) {
    for (const statistics::MetricSummary& summary : signature.metrics) {
      if (summary.metric.name == name && nth-- == 0) {
        return &summary;
      }
    }
    return nullptr;
  }

  Machine machine;
  std::unique_ptr<cli::Rig> rig;
  statistics::CompensatedSummarizer summarizer;
  std::int64_t now = kSecond;

 private:
  platform::FileSystem files_;
  std::int64_t first_ = 0;
  std::int64_t last_ = 0;
  std::uint64_t rounds_ = 0;
};

}  // namespace loadforge::m3

#endif
