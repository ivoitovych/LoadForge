// SPDX-License-Identifier: GPL-3.0-or-later
//
// The result of a run, put together from the controller's report, the
// timeline and the statistics over it -- over this module's own mocks of the
// timeline and the summarizer, whose readings of their contracts are checked
// against the providers' suites in contract_mocks_test.cpp.

#include "main/run_result.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "controller/results/aggregate.hpp"
#include "unit/controller/telemetry_mocks.hpp"

namespace loadforge::cli {
namespace {

constexpr std::int64_t kSecond = 1'000'000'000;

telemetry::Metric edac(const std::string& name, telemetry::Scope scope, std::uint32_t index) {
  return telemetry::Metric{name,
                           telemetry::Unit::kCount,
                           telemetry::Kind::kCounter,
                           scope,
                           index,
                           std::uint64_t{1} << 32U,
                           "edac"};
}

telemetry::Metric temperature() {
  return telemetry::Metric{"thermal.package",
                           telemetry::Unit::kCelsius,
                           telemetry::Kind::kGauge,
                           telemetry::Scope::kPackage,
                           0,
                           std::nullopt,
                           "hwmon"};
}

void count(mocks::MockTimeline& timeline, telemetry::SeriesId series, std::int64_t at,
           std::uint64_t raw) {
  ASSERT_TRUE(timeline.append(series, {at, telemetry::Counter{raw}}).has_value());
}

/// Corrected errors on controller 0: 0 at 1 s, 2 at 2 s, 2 at 3 s, 5 at 4 s,
/// 9 at 5 s. Phases: "run" from 1 s, "stop" from 3 s.
struct Sampled {
  mocks::MockTimeline timeline;
  mocks::MockSummarizer summarizer;
  telemetry::SeriesId corrected = 0;

  Sampled() {
    corrected =
        timeline.add_series(edac("edac.corrected", telemetry::Scope::kMemoryController, 0)).value();
    count(timeline, corrected, 1 * kSecond, 0);
    count(timeline, corrected, 2 * kSecond, 2);
    count(timeline, corrected, 3 * kSecond, 2);
    count(timeline, corrected, 4 * kSecond, 5);
    count(timeline, corrected, 5 * kSecond, 9);
    EXPECT_TRUE(timeline.mark_phase({1 * kSecond, "run"}).has_value());
    EXPECT_TRUE(timeline.mark_phase({3 * kSecond, "stop"}).has_value());
  }
};

TEST(PhaseWindowsTest, EachPhaseEndsOneNanosecondPastTheNextMarkAndTheLastPastTheEnd) {
  const Sampled run;
  const auto windows = phase_windows(run.timeline, 5 * kSecond);
  ASSERT_EQ(windows.size(), 2U);
  EXPECT_EQ(windows[0].label, "run");
  EXPECT_EQ(windows[0].window, (statistics::Window{1 * kSecond, (3 * kSecond) + 1}));
  EXPECT_EQ(windows[1].label, "stop");
  EXPECT_EQ(windows[1].window, (statistics::Window{3 * kSecond, (5 * kSecond) + 1}));
}

TEST(PhaseWindowsTest, ALongLabelIsKeptWholeInTheWindowAndInTheDelta) {
  mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  const std::string label = "settle-and-soak-at-the-thermal-limit";
  const auto series =
      timeline.add_series(edac("edac.corrected", telemetry::Scope::kMachine, 0)).value();
  count(timeline, series, 1 * kSecond, 1);
  count(timeline, series, 2 * kSecond, 3);
  ASSERT_TRUE(timeline.mark_phase({1 * kSecond, label}).has_value());
  const auto windows = phase_windows(timeline, 2 * kSecond);
  ASSERT_EQ(windows.size(), 1U);
  EXPECT_EQ(windows[0].label, label);
  const auto deltas = counter_by_phase(timeline, summarizer, "edac.corrected", 2 * kSecond);
  ASSERT_EQ(deltas.size(), 1U);
  EXPECT_EQ(deltas[0].phase, label);
}

TEST(PhaseWindowsTest, ATimelineWithNoMarkHasNoPhase) {
  const mocks::MockTimeline timeline;
  EXPECT_TRUE(phase_windows(timeline, 5 * kSecond).empty());
}

TEST(CounterByPhaseTest, EveryAdvanceIsInExactlyOnePhaseAndTheBoundaryPairIsNotLost) {
  const Sampled run;
  const auto deltas = counter_by_phase(run.timeline, run.summarizer, "edac.corrected", 5 * kSecond);
  ASSERT_EQ(deltas.size(), 2U);
  EXPECT_EQ(deltas[0].phase, "run");
  EXPECT_EQ(deltas[0].delta, 2U) << "1 s to 3 s: 0, 2, 2";
  EXPECT_EQ(deltas[1].phase, "stop");
  EXPECT_EQ(deltas[1].delta, 7U) << "3 s to 5 s: 2, 5, 9";
  EXPECT_EQ(deltas[0].delta + deltas[1].delta, 9U) << "the whole run advanced by 9";
  EXPECT_EQ(deltas[0].metric, "edac.corrected memory controller 0");
}

TEST(CounterByPhaseTest, AMachineScopedSeriesIsNamedByItsMetricAlone) {
  mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  const auto series =
      timeline.add_series(edac("edac.corrected", telemetry::Scope::kMachine, 0)).value();
  count(timeline, series, 1 * kSecond, 1);
  count(timeline, series, 2 * kSecond, 4);
  ASSERT_TRUE(timeline.mark_phase({1 * kSecond, "run"}).has_value());
  const auto deltas = counter_by_phase(timeline, summarizer, "edac.corrected", 2 * kSecond);
  ASSERT_EQ(deltas.size(), 1U);
  EXPECT_EQ(deltas[0].metric, "edac.corrected");
}

TEST(CounterByPhaseTest, OnlyTheNamedCounterThatMovedIsListed) {
  Sampled run;
  const auto still =
      run.timeline.add_series(edac("edac.uncorrected", telemetry::Scope::kMemoryController, 0))
          .value();
  count(run.timeline, still, 1 * kSecond, 7);
  count(run.timeline, still, 5 * kSecond, 7);
  const auto gauge = run.timeline.add_series(temperature()).value();
  ASSERT_TRUE(run.timeline.append(gauge, {1 * kSecond, telemetry::Gauge{40.0}}).has_value());
  const auto gone = run.timeline.add_series(edac("edac.corrected", telemetry::Scope::kDimm, 3));
  ASSERT_TRUE(gone.has_value());
  ASSERT_TRUE(run.timeline
                  .append(gone.value(),
                          {2 * kSecond, telemetry::Unavailable{topology::Availability::kVanished,
                                                               "/sys/x", "gone", "reload"}})
                  .has_value());
  EXPECT_TRUE(
      counter_by_phase(run.timeline, run.summarizer, "edac.uncorrected", 5 * kSecond).empty())
      << "a counter that did not move";
  EXPECT_TRUE(
      counter_by_phase(run.timeline, run.summarizer, "thermal.package", 5 * kSecond).empty())
      << "a gauge has no delta";
  EXPECT_EQ(counter_by_phase(run.timeline, run.summarizer, "edac.corrected", 5 * kSecond).size(),
            2U)
      << "the dimm that vanished adds nothing: absence is the signature's to report";
}

TEST(CounterByPhaseTest, APhaseThatCannotBeSummarisedIsSkipped) {
  const Sampled run;
  // The run ended before the last phase began: its window is empty.
  const auto deltas = counter_by_phase(run.timeline, run.summarizer, "edac.corrected", 2 * kSecond);
  ASSERT_EQ(deltas.size(), 1U);
  EXPECT_EQ(deltas[0].phase, "run");
}

// ---- assemble_result ------------------------------------------------------------

config::Config two_workloads(bool second_enabled) {
  config::Config config;
  config.run.mode = config::Mode::kStress;
  config.workloads.push_back(config::Workload{"first", config::Threads::all(), true, {}});
  config.workloads.push_back(
      config::Workload{"second", config::Threads::all(), second_enabled, {}});
  return config;
}

controller::RunReport report_of_two_workers() {
  controller::RunReport report;
  report.elapsed_ns = 4 * kSecond;
  report.workers.push_back(controller::WorkerReport{
      0, 11, controller::WorkerFate::kCompleted, "exited with code 0", "stop requested", 5, {}});
  report.workers.push_back(controller::WorkerReport{
      1, 12, controller::WorkerFate::kKilled, "killed by signal 9", "none", 2, {}});
  report.sampling.enabled = true;
  report.sampling.interval_ns = kSecond;
  report.sampling.rounds = 5;
  report.sampling.first_round_ns = 1 * kSecond;
  report.sampling.last_round_ns = 5 * kSecond;
  report.sampling.cost_ns = 4'000'000;
  return report;
}

TEST(AssembleResultTest, ARunIsItsReportItsCapabilitiesAndTheStatisticsOverItsTimeline) {
  const Sampled run;
  const config::Config config = two_workloads(true);
  const controller::RunReport report = report_of_two_workers();
  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report;
  inputs.fingerprint.loadforge_version = "9.9.9";
  inputs.capabilities.sources.push_back({"edac", {}});
  inputs.timeline = &run.timeline;
  inputs.summarizer = &run.summarizer;
  const auto result = assemble_result(inputs);
  ASSERT_TRUE(result.has_value()) << result.error();
  const results::RunResult& value = result.value();
  EXPECT_EQ(value.schema_version, results::kSchemaVersion);
  EXPECT_EQ(value.mode, config::Mode::kStress);
  EXPECT_EQ(value.elapsed_ns, 4 * kSecond);
  EXPECT_EQ(value.fingerprint.loadforge_version, "9.9.9");
  ASSERT_EQ(value.capabilities.sources.size(), 1U);
  EXPECT_EQ(value.signature.window, (statistics::Window{1 * kSecond, (5 * kSecond) + 1}));
  ASSERT_EQ(value.signature.metrics.size(), 1U);
  const auto* counter = std::get_if<statistics::CounterSummary>(&value.signature.metrics[0].value);
  ASSERT_NE(counter, nullptr);
  EXPECT_EQ(counter->delta, 9U) << "the closing reading is in the window";
  ASSERT_EQ(value.workers.size(), 2U);
  EXPECT_EQ(value.workers[1].slot, 1U);
  EXPECT_EQ(value.workers[1].fate, controller::WorkerFate::kKilled);
  EXPECT_EQ(value.workers[1].termination, "killed by signal 9");
  EXPECT_FALSE(value.workers[1].cpu.has_value());
  EXPECT_EQ(value.observers.sample_interval_ns, kSecond);
  ASSERT_TRUE(value.observers.controller_overhead.has_value());
  EXPECT_DOUBLE_EQ(*value.observers.controller_overhead, 0.001);
  EXPECT_FALSE(value.observers.journal_flush_interval_ns.has_value());
  EXPECT_EQ(value.verdict, results::Verdict::kIncomplete);
}

TEST(AssembleResultTest, OnlyEnabledWorkloadsAreInTheResultAndNoneClaimsAVerification) {
  const Sampled run;
  const config::Config config = two_workloads(false);
  const controller::RunReport report = report_of_two_workers();
  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report;
  inputs.timeline = &run.timeline;
  inputs.summarizer = &run.summarizer;
  const auto result = assemble_result(inputs);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value().workloads.size(), 1U);
  const results::WorkloadResult& workload = result.value().workloads[0];
  EXPECT_EQ(workload.descriptor.name, "first");
  EXPECT_EQ(workload.tally, verification::Tally{}) << "the null workload computes nothing";
  EXPECT_EQ(workload.duty, (verification::DutyCycle{0, 0})) << "and schedules no check";
  EXPECT_TRUE(workload.findings.empty());
}

TEST(AssembleResultTest,
     AVerifyingWorkersTallyAndFindingsAreTheResultsAndTheIdleWorkloadsStandDown) {
  const Sampled run;
  const config::Config config = two_workloads(true);
  controller::RunReport report = report_of_two_workers();
  controller::WorkerWork work;
  work.descriptor.name = "compression";
  work.cpu = 3;
  work.logical_bytes = 1000;
  report.workers[0].work = work;
  report.workers[0].tally_seen = true;
  report.workers[0].tally.units_computed = 5;
  report.workers[0].tally.oracle_checked = 5;
  report.workers[0].tally.oracle_blocks = 5;
  report.workers[0].tally.integrity_checked = 5;
  verification::ExactBitCorruption corruption;
  corruption.check = "decoded output against input";
  controller::FindingReport finding;
  finding.record = corruption;
  report.workers[0].findings = {finding};
  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report;
  inputs.timeline = &run.timeline;
  inputs.summarizer = &run.summarizer;
  const auto result = assemble_result(inputs);
  ASSERT_TRUE(result.has_value()) << result.error();
  ASSERT_EQ(result.value().workloads.size(), 1U) << "what was verified, not the configured names";
  EXPECT_EQ(result.value().workloads[0].descriptor.name, "compression");
  EXPECT_EQ(result.value().workloads[0].tally.units_computed, 5U);
  EXPECT_EQ(result.value().workloads[0].findings.size(), 1U);
  EXPECT_EQ(result.value().workers[0].cpu, 3U);
  EXPECT_EQ(result.value().verdict, results::Verdict::kFindings);
  EXPECT_EQ(result.value().signature.window, (statistics::Window{1 * kSecond, (5 * kSecond) + 1}))
      << "and the telemetry is the run's all the same";
}

TEST(AssembleResultTest, ARunThatTookNoRoundHasNoSignatureAndSaysNothingAboutOverhead) {
  const Sampled run;
  const config::Config config = two_workloads(true);
  controller::RunReport report = report_of_two_workers();
  report.sampling = controller::SamplingReport{};
  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report;
  inputs.timeline = &run.timeline;
  inputs.summarizer = &run.summarizer;
  const auto result = assemble_result(inputs);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value().signature.metrics.empty());
  EXPECT_EQ(result.value().signature.window, (statistics::Window{0, 0}));
  EXPECT_EQ(result.value().observers.sample_interval_ns, 0);
  EXPECT_FALSE(result.value().observers.controller_overhead.has_value())
      << "an observer that was not there cost nothing to state";
}

TEST(AssembleResultTest, AZeroElapsedTimeStatesNoOverheadRatherThanDividingByIt) {
  const Sampled run;
  const config::Config config = two_workloads(true);
  controller::RunReport report = report_of_two_workers();
  report.elapsed_ns = 0;
  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report;
  inputs.timeline = &run.timeline;
  inputs.summarizer = &run.summarizer;
  const auto result = assemble_result(inputs);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result.value().observers.controller_overhead.has_value());
}

TEST(AssembleResultTest, AnObserverThatCostMoreThanTheRunIsStatedAsAWholeCpuAtMost) {
  const Sampled run;
  const config::Config config = two_workloads(true);
  controller::RunReport report = report_of_two_workers();
  report.sampling.cost_ns = 40 * kSecond;
  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report;
  inputs.timeline = &run.timeline;
  inputs.summarizer = &run.summarizer;
  const auto result = assemble_result(inputs);
  ASSERT_TRUE(result.has_value());
  EXPECT_DOUBLE_EQ(*result.value().observers.controller_overhead, 1.0)
      << "the contract's range is 0 to 1";
}

TEST(AssembleResultTest, RoundsThatContradictThemselvesAreAnErrorNamingTheFault) {
  const Sampled run;
  const config::Config config = two_workloads(true);
  controller::RunReport report = report_of_two_workers();
  report.sampling.first_round_ns = 9 * kSecond;
  report.sampling.last_round_ns = 2 * kSecond;
  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report;
  inputs.timeline = &run.timeline;
  inputs.summarizer = &run.summarizer;
  const auto result = assemble_result(inputs);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("empty or inverted window"), std::string::npos) << result.error();
}

// ---- summary_text ---------------------------------------------------------------

TEST(SummaryTextTest, ItStatesTheVerdictTheObserverAndWhatEachDeathLeftBehind) {
  const Sampled run;
  const config::Config config = two_workloads(true);
  controller::RunReport report = report_of_two_workers();
  report.sampling.points = 5;
  report.sampling.reaps = {controller::Reap{1, 3 * kSecond}};
  ResultInputs inputs;
  inputs.config = &config;
  inputs.report = &report;
  inputs.timeline = &run.timeline;
  inputs.summarizer = &run.summarizer;
  const auto result = assemble_result(inputs);
  ASSERT_TRUE(result.has_value());
  const std::string text = summary_text(result.value(), report, run.timeline, run.summarizer, {});
  EXPECT_NE(text.find("verdict: incomplete\n"), std::string::npos) << text;
  EXPECT_NE(text.find("telemetry: 5 round(s) every 1.000 s, 5 point(s) over 1 series; "
                      "sampling took 0.004 s (0.1000 % of one CPU)\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("worker 1 reaped at 2.000 s into the telemetry; the timeline holds 3 "
                      "point(s) up to then\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("edac: edac.corrected memory controller 0 advanced by 2 in phase \"run\"\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("edac: edac.corrected memory controller 0 advanced by 7 in phase \"stop\"\n"),
            std::string::npos)
      << text;
  EXPECT_EQ(text.find("fingerprint:"), std::string::npos);
}

TEST(SummaryTextTest, ARunWithoutTelemetrySaysSoAndOneWithoutEdacSaysNothingOfIt) {
  const mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  const controller::RunReport report;
  results::RunResult result;
  result.verdict = results::Verdict::kClean;
  const std::string text = summary_text(result, report, timeline, summarizer, {});
  EXPECT_EQ(text, "verdict: clean\ntelemetry: not sampled\n");
}

TEST(SummaryTextTest, ARunTheCeilingEndedSaysSoRightBelowItsVerdict) {
  const mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  controller::RunReport report;
  controller::TemperatureStop stop;
  stop.sensor = "thermal.package 0 (hwmon:coretemp)";
  stop.celsius = 96.5;
  stop.ceiling_c = 95;
  stop.at_ns = 2'250'000'000;
  report.temperature_stop = stop;
  results::RunResult result;
  result.verdict = results::Verdict::kClean;
  EXPECT_EQ(summary_text(result, report, timeline, summarizer, {}),
            "verdict: clean\n"
            "ended by the temperature ceiling: thermal.package 0 (hwmon:coretemp) read 96.5 C, "
            "at or above the ceiling of 95 C, 2.250 s into the run\n"
            "telemetry: not sampled\n");
}

TEST(SummaryTextTest, ARunASignalStoppedSaysSoAndWhichExitCodeThatIs) {
  const mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  controller::RunReport report;
  report.stop_signal = SIGINT;
  results::RunResult result;
  result.verdict = results::Verdict::kClean;
  EXPECT_EQ(summary_text(result, report, timeline, summarizer, {}),
            "verdict: clean\n"
            "the run was stopped by SIGINT: its exit code is 130\n"
            "telemetry: not sampled\n");
  report.stop_signal = SIGTERM;
  report.stop_forced = true;
  EXPECT_EQ(summary_text(result, report, timeline, summarizer, {}),
            "verdict: clean\n"
            "the run was stopped by SIGTERM; a later signal killed the workers at once: its "
            "exit code is 143\n"
            "telemetry: not sampled\n");
}

TEST(SummaryTextTest, AnOverheadThatCouldNotBeComputedIsLeftOutAndAFaultIsStated) {
  const mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  controller::RunReport report;
  report.sampling.enabled = true;
  report.sampling.interval_ns = kSecond;
  report.sampling.fault = "sampling stopped: capacity reached (full)";
  const results::RunResult result;
  const std::string text = summary_text(result, report, timeline, summarizer, {});
  EXPECT_NE(
      text.find("0 round(s) every 1.000 s, 0 point(s) over 0 series; sampling took 0.000 s\n"),
      std::string::npos)
      << text;
  EXPECT_NE(text.find("telemetry fault: sampling stopped: capacity reached (full)\n"),
            std::string::npos);
}

TEST(SummaryTextTest, EdacThatCountedNothingSaysSoAndOnlyUncorrectedIsStillEdac) {
  mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  const auto series =
      timeline.add_series(edac("edac.uncorrected", telemetry::Scope::kMemoryController, 0)).value();
  count(timeline, series, 1 * kSecond, 0);
  count(timeline, series, 2 * kSecond, 0);
  ASSERT_TRUE(timeline.mark_phase({1 * kSecond, "run"}).has_value());
  controller::RunReport report;
  report.sampling.enabled = true;
  report.sampling.last_round_ns = 2 * kSecond;
  const results::RunResult result;
  const std::string text = summary_text(result, report, timeline, summarizer, {});
  EXPECT_NE(text.find("edac: no error counted in any phase\n"), std::string::npos) << text;
}

TEST(SummaryTextTest, UncorrectedErrorsAreListedByPhaseToo) {
  mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  const auto series =
      timeline.add_series(edac("edac.uncorrected", telemetry::Scope::kMemoryController, 1)).value();
  count(timeline, series, 1 * kSecond, 0);
  count(timeline, series, 2 * kSecond, 1);
  ASSERT_TRUE(timeline.mark_phase({1 * kSecond, "run"}).has_value());
  controller::RunReport report;
  report.sampling.enabled = true;
  report.sampling.last_round_ns = 2 * kSecond;
  const results::RunResult result;
  const std::string text = summary_text(result, report, timeline, summarizer, {});
  EXPECT_NE(
      text.find("edac: edac.uncorrected memory controller 1 advanced by 1 in phase \"run\"\n"),
      std::string::npos)
      << text;
}

TEST(SummaryTextTest, EveryFingerprintFieldTheMachineWouldNotGiveIsNamedWithItsReason) {
  const mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  const controller::RunReport report;
  const results::RunResult result;
  results::FingerprintGap gap;
  gap.field = "microcode";
  gap.why = topology::Unavailable{topology::Availability::kAbsent, "/proc/cpuinfo", "no line"};
  const std::string text = summary_text(result, report, timeline, summarizer, {gap});
  EXPECT_NE(text.find("fingerprint: microcode not recorded: "), std::string::npos) << text;
  EXPECT_NE(text.find("/proc/cpuinfo"), std::string::npos) << text;
}

}  // namespace
}  // namespace loadforge::cli

namespace loadforge::cli {
namespace {

statistics::MetricSummary edac_summary(const std::string& name, const statistics::Summary& value,
                                       std::uint64_t unavailable) {
  statistics::MetricSummary summary;
  summary.metric = edac(name, telemetry::Scope::kMemoryController, 0);
  summary.value = value;
  summary.unavailable_samples = unavailable;
  return summary;
}

std::string edac_text(const std::vector<statistics::MetricSummary>& metrics) {
  mocks::MockTimeline timeline;
  const mocks::MockSummarizer summarizer;
  const auto series =
      timeline.add_series(edac("edac.corrected", telemetry::Scope::kMemoryController, 0)).value();
  count(timeline, series, 1 * kSecond, 0);
  count(timeline, series, 2 * kSecond, 0);
  EXPECT_TRUE(timeline.mark_phase({1 * kSecond, "run"}).has_value());
  controller::RunReport report;
  report.sampling.enabled = true;
  report.sampling.last_round_ns = 2 * kSecond;
  results::RunResult result;
  result.signature.metrics = metrics;
  return summary_text(result, report, timeline, summarizer, {});
}

TEST(SummaryTextTest, AnEdacCounterThatResetOrDroppedOutSaysTheVerdictIsIncompleteThere) {
  statistics::CounterSummary reset;
  reset.samples = 3;
  reset.resets = 2;
  const std::string by_reset = edac_text({edac_summary("edac.corrected", reset, 0)});
  EXPECT_NE(by_reset.find("edac: edac.corrected memory controller 0 was not read throughout (2 "
                          "reset(s), 0 unavailable sample(s)): the verdict is incomplete there\n"),
            std::string::npos)
      << by_reset;
  const std::string by_gap =
      edac_text({edac_summary("edac.uncorrected", statistics::CounterSummary{}, 4)});
  EXPECT_NE(by_gap.find("edac: edac.uncorrected memory controller 0 was not read throughout (0 "
                        "reset(s), 4 unavailable sample(s))"),
            std::string::npos)
      << by_gap;
  const telemetry::Unavailable none{};
  const std::string by_state = edac_text({edac_summary("edac.corrected", none, 0)});
  EXPECT_EQ(by_state.find("not read throughout"), std::string::npos) << by_state;
}

TEST(SummaryTextTest, AnEdacCounterReadThroughoutAndOtherMetricsAddNoIncompleteNote) {
  const statistics::MetricSummary other =
      edac_summary("thermal.package", statistics::CounterSummary{}, 9);
  const std::string text =
      edac_text({edac_summary("edac.corrected", statistics::CounterSummary{}, 0), other});
  EXPECT_EQ(text.find("not read throughout"), std::string::npos) << text;
}

}  // namespace
}  // namespace loadforge::cli
