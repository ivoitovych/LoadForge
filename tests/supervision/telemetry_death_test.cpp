// SPDX-License-Identifier: GPL-3.0-or-later
//
// T7, M3 exit criterion 2: a worker killed mid-run leaves the telemetry before
// its death in the result. The controller samples in its own loop, so the
// timeline is the controller's and does not die with a worker; what this
// proves is that the whole path -- real sources over this machine, the
// sampler, the timeline, the statistics, the result, the JSON writer -- keeps
// what was read before the death and goes on reading after it, with two real
// workers, one of them killed from outside while the other completes.

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "config/config.hpp"
#include "controller/orchestration.hpp"
#include "controller/report.hpp"
#include "controller/results/fingerprint.hpp"
#include "core/duration.hpp"
#include "main/run_result.hpp"
#include "main/telemetry_rig.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "reporters/json/json_writer.hpp"
#include "statistics/summarizer.hpp"

namespace loadforge::controller {
namespace {

constexpr std::int64_t kMs = 1'000'000;

std::vector<pid_t> pids_after_colon(std::string_view line) {
  std::vector<pid_t> pids;
  std::istringstream rest{std::string{line.substr(line.find("pids") + 4)}};
  pid_t pid = 0;
  while (rest >> pid) {
    pids.push_back(pid);
  }
  return pids;
}

/// Kills the second worker `after` into the run, from a thread started once
/// both workers exist (so the controller's forks are done).
class KillingReporter final : public Reporter {
 public:
  explicit KillingReporter(std::chrono::milliseconds after) : after_(after) {}
  ~KillingReporter() override {
    if (killer_.joinable()) {
      killer_.join();
    }
  }
  KillingReporter(const KillingReporter&) = delete;
  KillingReporter(KillingReporter&&) = delete;
  KillingReporter& operator=(const KillingReporter&) = delete;
  KillingReporter& operator=(KillingReporter&&) = delete;

  void run_starting(const RunPlan& /*plan*/) override {}
  void event(std::int64_t /*elapsed_ns*/, std::string_view line) override {
    events.emplace_back(line);
    if (line.substr(0, 7) == "spawned") {
      const std::vector<pid_t> pids = pids_after_colon(line);
      victim = pids.at(1);
      killer_ = std::thread([pid = victim, after = after_] {
        std::this_thread::sleep_for(after);
        ::kill(pid, SIGKILL);
      });
    }
  }
  void run_finished(const RunReport& /*report*/) override {}

  pid_t victim = 0;
  std::vector<std::string> events;

 private:
  std::chrono::milliseconds after_;
  std::thread killer_;
};

TEST(TelemetrySupervisionTest, AWorkerKilledMidRunLeavesTheTelemetryBeforeItsDeathInTheResult) {
  platform::RealSyscalls syscalls;
  platform::FileSystem files(syscalls);
  auto rig = cli::make_rig(syscalls, files, cli::MachineRoots{}, 100'000);
  ASSERT_TRUE(rig.has_value()) << rig.error().detail;

  config::Config config;
  config.run.duration = core::Duration{1800};
  config.workloads.push_back(config::Workload{"first", config::Threads::all(), true, {}});
  config.workloads.push_back(config::Workload{"second", config::Threads::all(), true, {}});

  RunOptions options;
  options.executable = LOADFORGE_BINARY;
  options.interval_ns = 20 * kMs;
  options.stale_after_ns = 2000 * kMs;
  options.grace = Grace{500 * kMs, 500 * kMs};
  options.sampler = &rig.value()->sampler();
  options.timeline = &rig.value()->timeline();
  options.sample_interval_ns = 50 * kMs;

  KillingReporter reporter(std::chrono::milliseconds{700});
  Controller controller(syscalls, reporter);
  auto report = controller.run(config, options);
  ASSERT_TRUE(report.has_value()) << describe(report.error());
  ASSERT_EQ(report.value().workers.size(), 2U);
  EXPECT_EQ(report.value().workers[0].fate, WorkerFate::kCompleted);
  ASSERT_EQ(report.value().workers[1].fate, WorkerFate::kKilled);

  const SamplingReport& sampling = report.value().sampling;
  EXPECT_FALSE(sampling.fault.has_value()) << sampling.fault.value_or("");
  ASSERT_EQ(sampling.reaps.size(), 2U);
  const Reap* death = nullptr;
  for (const Reap& reap : sampling.reaps) {
    if (reap.slot == 1) {
      death = &reap;
    }
  }
  ASSERT_NE(death, nullptr);
  const std::int64_t before_death_ns = death->at_ns - sampling.first_round_ns;
  EXPECT_GT(before_death_ns, 600 * kMs) << "the kill came 700 ms into the run";
  EXPECT_LT(death->at_ns, sampling.last_round_ns) << "sampling went on after the death";

  // What the timeline held when the worker died.
  const telemetry::TimelineView& timeline = rig.value()->timeline();
  ASSERT_GT(timeline.series_count(), 0U);
  std::size_t before = 0;
  std::size_t after = 0;
  for (std::size_t series = 0; series < timeline.series_count(); ++series) {
    for (const telemetry::Point& point :
         timeline.points(static_cast<telemetry::SeriesId>(series))) {
      (point.timestamp_ns <= death->at_ns ? before : after)++;
    }
  }
  EXPECT_GE(before, timeline.series_count() * 10) << "at 20 Hz, 700 ms is at least ten rounds";
  EXPECT_GT(after, 0U);

  // The result carries all of it: the signature's window spans the death.
  const statistics::CompensatedSummarizer summarizer;
  platform::FileSystem machine(syscalls);
  cli::ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report.value();
  inputs.fingerprint = results::read_fingerprint(
                           machine, "", results::FingerprintContext{results::this_build(), {}, {}})
                           .fingerprint;
  inputs.capabilities = rig.value()->capabilities();
  inputs.timeline = &timeline;
  inputs.summarizer = &summarizer;
  const auto result = cli::assemble_result(inputs);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_GT(result.value().signature.window.end_ns, death->at_ns);
  EXPECT_LE(result.value().signature.window.start_ns, sampling.first_round_ns);
  EXPECT_EQ(result.value().signature.metrics.size(), timeline.series_count());
  std::uint64_t most_samples = 0;
  for (const statistics::MetricSummary& summary : result.value().signature.metrics) {
    if (const auto* gauge = std::get_if<statistics::GaugeSummary>(&summary.value)) {
      most_samples = std::max(most_samples, gauge->samples);
    }
  }
  EXPECT_GE(most_samples, 10U) << "a gauge's summary holds the readings from before the death";
  EXPECT_EQ(result.value().workers[1].fate, WorkerFate::kKilled);
  EXPECT_EQ(result.value().verdict, results::Verdict::kIncomplete)
      << "a killed worker and nothing verified: not a clean run";

  std::ostringstream json;
  ASSERT_TRUE(reporters::JsonWriter{}.write(result.value(), json).has_value());
  EXPECT_NE(json.str().find("\"killed\""), std::string::npos);
}

}  // namespace
}  // namespace loadforge::controller
