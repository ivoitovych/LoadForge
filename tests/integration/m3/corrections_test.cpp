// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE CONTRACT CORRECTIONS OF THE M3 INTEGRATION, FOLLOWED THROUGH THE STACK.
//
// The integration of the M3 components found seven places where two components
// read one contract differently (docs/journal/21-integration-m3.md). Each was
// first written down as a test that asserted what the contract said and failed
// against the components as built, registered WILL_FAIL. The seven contract
// changes are made (docs/journal/m3-contract-corrections-*.md); these are the
// same seven tests, asserting the corrected contracts through the real stack,
// and they pass. A test is named
//
//   <Component>_<ContractFile>_<Clause>
//
// after the component that was wrong against the clause it was wrong against.

#include <gtest/gtest.h>
#include <linux/perf_event.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "controller/results/aggregate.hpp"
#include "stack.hpp"
#include "telemetry/capability/capability.hpp"
#include "telemetry/sampler.hpp"

namespace loadforge::m3 {
namespace {

using statistics::CounterSummary;

const CounterSummary* counter_of(const statistics::LoadSignature& signature,
                                 const std::string& name, std::size_t nth = 0) {
  const statistics::MetricSummary* summary = Stack::find(signature, name, nth);
  return summary == nullptr ? nullptr : std::get_if<CounterSummary>(&summary->value);
}

// 1. sample.hpp, Kind and Unit: a counter's raw value is in its metric's unit. The
// power source reported the metric as joules and the raw value in microjoules, so
// +20 J was "delta 20000000 joules". The metric is now in microjoules.
TEST(Correction, PowerSource_SampleHpp_AnEnergyCounterIsInTheUnitItsMetricNames) {
  Stack stack;
  stack.machine.install_everything();
  stack.machine.set("/sys/class/powercap/intel-rapl:0/energy_uj", "1000000");
  stack.start();
  stack.round();
  stack.machine.set("/sys/class/powercap/intel-rapl:0/energy_uj", "21000000");  // +20 J
  stack.round();
  const auto signature = stack.signature();
  const auto* energy = counter_of(signature, "power.package");
  ASSERT_NE(energy, nullptr);
  const statistics::MetricSummary* summary = Stack::find(signature, "power.package");
  EXPECT_EQ(summary->metric.unit, telemetry::Unit::kMicrojoules);
  EXPECT_EQ(energy->delta, 20'000'000U) << "20 J, in the unit the metric names";
  const std::string json = Stack::json(stack.result());
  EXPECT_NE(json.find("\"unit\": \"microjoules\""), std::string::npos) << json;
  EXPECT_EQ(json.find("\"unit\": \"joules\""), std::string::npos) << "no metric says joules";
}

// 2. sample.hpp, Kind: a counter only ever increases. A count scaled up for the
// time the event was off the PMU is an estimate and goes down when the next round's
// count is exact. The counter is the raw count; the share of time on the PMU is a
// gauge beside it.
TEST(Correction, CountersSource_SampleHpp_ACounterOnlyEverIncreases) {
  Stack stack;
  stack.machine.install_everything();
  stack.machine.syscalls().script_perf_reads(
      kPerfHardware, PERF_COUNT_HW_CPU_CYCLES,
      {{platform::PerfCount{1000, 2000, 1000}, 0},    // shared PMU: 1000 counted in half the time
       {platform::PerfCount{1500, 2000, 2000}, 0}});  // exclusive again: 1500, truly more
  stack.start();
  stack.rounds(2);
  const auto signature = stack.signature();
  const auto* cycles = counter_of(signature, "counters.cycles");
  ASSERT_NE(cycles, nullptr);
  EXPECT_EQ(cycles->delta, 500U) << "1000 counted, then 1500 counted";
  EXPECT_EQ(cycles->resets, 0U);
  const statistics::MetricSummary* share = Stack::find(signature, "counters.pmu_share");
  ASSERT_NE(share, nullptr);
  const auto* gauge = std::get_if<statistics::GaugeSummary>(&share->value);
  ASSERT_NE(gauge, nullptr);
  EXPECT_EQ(gauge->min, 0.5) << "the first round saw the event on the PMU half of the time";
  EXPECT_EQ(gauge->max, 1.0);
}

// 3. sample.hpp, Metric::modulus: a counter that cannot wrap within a run has no
// modulus, and a step down in it is a reset: a gap, not 2^32 - 5 corrected errors.
TEST(Correction, EdacSource_SampleHpp_AStepDownInACounterThatCannotWrapInARunIsAReset) {
  Stack stack;
  stack.machine.install_everything();
  stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", "5");
  stack.start();
  stack.round();
  stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", "0");  // reset_counters
  stack.round();
  stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", "3");
  stack.round();
  const auto signature = stack.signature();
  const auto* corrected = counter_of(signature, "edac.corrected");
  ASSERT_NE(corrected, nullptr);
  EXPECT_EQ(corrected->delta, 3U) << "what was counted after the reset, from the new baseline";
  EXPECT_EQ(corrected->resets, 1U);
  EXPECT_FALSE(Stack::find(signature, "edac.corrected")->metric.modulus.has_value());
}

// 4. sample.hpp, "never a zero standing in for a value": a context switch is
// counted in the kernel, so the event is not opened with the kernel excluded. The
// evidence is in docs/journal/m3-contract-corrections-*.md (a standalone program on
// Linux 6.18: 0 0 0 0 0 with exclude_kernel, 1 2 3 4 5 without it).
TEST(Correction, CountersSource_SampleHpp_ASoftwareEventIsNotAskedToExcludeTheKernelItCountsIn) {
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  bool software = false;
  for (const platform::PerfEventSpec& spec : stack.machine.syscalls().perf_opens()) {
    if (spec.type == kPerfSoftware) {
      software = true;
      EXPECT_FALSE(spec.user_only) << "config " << spec.config;
    } else {
      EXPECT_TRUE(spec.user_only) << "what the CPU counts is counted in user space";
    }
  }
  EXPECT_TRUE(software);
}

// 5. signature.hpp: the windows of a run's phases are built by phase_window(), which
// closes a phase on the reading at the next mark, so a counter's advance across a
// boundary belongs to the phase it happened in and the phases sum to the run.
TEST(Correction, Statistics_SignatureHpp_PhasesTakenAtTheirMarksSumToTheRun) {
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  const std::int64_t first = stack.now;
  const auto count = [&](const char* value) {
    stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", value);
    stack.round();
  };
  count("0");  // 1 s
  count("1");  // 2 s
  ASSERT_TRUE(stack.rig->timeline().mark_phase({stack.now, "stop"}).has_value());
  count("4");  // 3 s, the mark: +3 since the last reading
  count("5");  // 4 s
  const std::int64_t last = stack.now - kSecond;
  const auto whole =
      stack.summarizer.summarize(stack.timeline(), statistics::run_window(first, last));
  const std::uint64_t run = counter_of(whole.value(), "edac.corrected")->delta;
  std::uint64_t phases = 0;
  const auto marks = stack.timeline().phases();
  ASSERT_EQ(marks.size(), 1U);
  const statistics::Window windows[] = {{first, marks[0].start_ns + statistics::kClosingMargin},
                                        statistics::phase_window(marks, 0, last)};
  // The first window above is the run phase before the mark was made, closed on the
  // mark's reading: [first, mark]. The second is the phase the mark starts.
  for (const statistics::Window& window : windows) {
    phases +=
        counter_of(stack.summarizer.summarize(stack.timeline(), window).value(), "edac.corrected")
            ->delta;
  }
  EXPECT_EQ(run, 5U);
  EXPECT_EQ(phases, run) << "the advance across the boundary belongs to a phase";
}

// 6. result.hpp, Verdict::kIncomplete and signature.hpp: a counter blind for part
// of the run is not a claim of cleanliness. The verdict reads unavailable_samples
// and resets of the hardware error counters.
TEST(Correction, Results_ResultHpp_ACounterBlindForPartOfTheRunIsNotAClaimOfCleanliness) {
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  stack.round();
  stack.machine.remove("/sys/devices/system/edac/mc/mc0/ce_count");
  stack.rounds(2);  // the controller's counters go unread: a driver reload
  stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", "40");  // errors in the gap
  stack.rounds(2);
  results::RunResult result = stack.result();
  result.workloads[0].tally = verification::Tally{100, 10, 10, 0, 0, 10, 0};
  result.workers[0].fate = controller::WorkerFate::kCompleted;
  results::seal(result);
  EXPECT_EQ(result.verdict, results::Verdict::kIncomplete)
      << "hardware errors may have been corrected while the counter could not be read";
}

// 6b. The same through a reset: counters cleared mid-run, so the advance across the
// reset is not known.
TEST(Correction, Results_ResultHpp_ACounterThatWasResetIsNotAClaimOfCleanlinessEither) {
  Stack stack;
  stack.machine.install_everything();
  stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", "9");
  stack.start();
  stack.round();
  stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", "0");
  stack.rounds(2);
  results::RunResult result = stack.result();
  result.workloads[0].tally = verification::Tally{100, 10, 10, 0, 0, 10, 0};
  result.workers[0].fate = controller::WorkerFate::kCompleted;
  results::seal(result);
  EXPECT_EQ(result.verdict, results::Verdict::kIncomplete);
  EXPECT_NE(Stack::json(result).find("\"resets\": 1"), std::string::npos);
}

// 7. source.hpp, sampler.hpp, capability.hpp: the sampler is the only prober, and
// the capability report is its answer. A source is probed once however many times the
// report is asked for and rendered.
class CountingSource final : public telemetry::Source {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "counting"; }
  std::vector<telemetry::MetricStatus> probe() override {
    ++probes;
    return {{telemetry::Metric{"counting.value", telemetry::Unit::kCount, telemetry::Kind::kGauge,
                               telemetry::Scope::kMachine, 0, std::nullopt, "test"},
             std::nullopt}};
  }
  void sample(std::vector<telemetry::Reading>& out) override {
    out.emplace_back(telemetry::Gauge{1});
  }
  int probes = 0;
};

TEST(Correction, Capability_SourceHpp_ASourceIsProbedOnceWhoeverReportsItsCapabilities) {
  auto source = std::make_unique<CountingSource>();
  CountingSource* counting = source.get();
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::move(source));
  telemetry::BoundedTimeline timeline(100);
  const auto sampler = telemetry::make_sampler(std::move(sources), timeline);
  const auto report = sampler->probe();
  ASSERT_TRUE(report.has_value());
  const std::string text = telemetry::render_capabilities(report.value());
  EXPECT_NE(text.find("counting: 1 of 1 present"), std::string::npos) << text;
  ASSERT_TRUE(sampler->probe().has_value());
  ASSERT_TRUE(sampler->round(1).has_value());
  EXPECT_EQ(counting->probes, 1);
}

}  // namespace
}  // namespace loadforge::m3
