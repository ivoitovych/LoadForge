// SPDX-License-Identifier: GPL-3.0-or-later
//
// WHERE TWO COMPONENTS READ ONE CONTRACT DIFFERENTLY.
//
// Each test below states what the contract says, in the contract's own words,
// and asserts it, against the components as their streams built them. Each
// FAILS today, by design: the suite is registered with WILL_FAIL (see this
// directory's CMakeLists.txt), so the build stays green while the disagreement
// stands, and goes red the day a component is corrected, which is the moment
// the test must move from here to a passing suite. A test is named
//
//   <Component>_<ContractFile>_<Clause>
//
// after the component that is wrong against the clause it is wrong against.
// Nothing in src/ outside this stream's own directories is edited to hide one
// of them (docs/journal/21-integration-m3.md has each, with its evidence).

#include <gtest/gtest.h>
#include <linux/perf_event.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
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

// sample.hpp, Metric::unit and Counter: "Unit::kJoules" on a counter whose raw
// value is "energy in microjoules". The power source reports the metric as
// joules and the raw value in microjoules; the statistics sum the raw values
// and the writers print them under the metric's unit. No component scales, so
// a result states 20 J as "delta 20000000 joules".
TEST(Disagreement, PowerSource_SampleHpp_AnEnergyCounterIsInTheUnitItsMetricNames) {
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
  ASSERT_EQ(summary->metric.unit, telemetry::Unit::kJoules);
  EXPECT_EQ(energy->delta, 20U) << "the metric says joules; the delta says " << energy->delta
                                << " of something else";
}

// sample.hpp, Counter: "A counter only ever increases, modulo its wrap".
// The counters source reports a perf count scaled to the time its event was
// enabled when the PMU was shared with other events, and an estimate is not
// monotonic: scaled up in one round, exact in the next, it goes down, and
// counter_delta reads every step down as a wrap at 2^64.
TEST(Disagreement, CountersSource_SampleHpp_ACounterOnlyEverIncreases) {
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
  EXPECT_LT(cycles->delta, 1'000'000U)
      << "between 1000 counted (estimated 2000) and 1500 counted the cycles advanced by a few "
         "thousand at most, not by "
      << cycles->delta;
}

// sample.hpp, "EVERY METRIC IS NULLABLE": "A reading is a value or an Unavailable,
// never a zero standing in for one". The counters source opens every event with
// exclude_kernel set, software events included; a context switch is counted in
// the kernel, so on a kernel that grants the event it reads zero for ever --
// present, valid, finite, and never anything else. Established on this
// machine's kernel (Linux 6.18) by a standalone perf_event_open program: with
// exclude_kernel set the count stays 0 across five 20 ms sleeps, without it the
// count is 1, 2, 3, 4, 5. (The real-kernel run of the stack shows the same:
// delta 0 over a run that slept.) Asserted here on what the source asks the
// kernel for, which does not depend on the machine granting the event.
TEST(Disagreement, CountersSource_SampleHpp_ASoftwareEventIsNotAskedToExcludeTheKernelItCountsIn) {
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  bool opened = false;
  for (const platform::PerfEventSpec& spec : stack.machine.syscalls().perf_opens()) {
    if (spec.type == kPerfSoftware) {
      opened = true;
      EXPECT_FALSE(spec.user_only) << "config " << spec.config
                                   << ": counted in the kernel, so "
                                      "excluding the kernel leaves nothing to count";
    }
  }
  EXPECT_TRUE(opened);
}

// sample.hpp, counter_delta and the EDAC source's modulus: a counter that
// goes down is a wrap, "(current - previous) modulo modulus". The EDAC source
// declares 2^32 because the kernel's counters are 32 bits wide. At one
// error a second a 32-bit counter wraps in 136 years; what makes it go down
// is a reset (writing to mc/reset_counters, a driver reloaded), and the
// advance reported is then 4,294,967,291 corrected errors that did not happen.
TEST(Disagreement, EdacSource_SampleHpp_AStepDownInACounterThatCannotWrapInARunIsAReset) {
  Stack stack;
  stack.machine.install_everything();
  stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", "5");
  stack.start();
  stack.round();
  stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", "0");  // reset_counters
  stack.round();
  const auto signature = stack.signature();
  const auto* corrected = counter_of(signature, "edac.corrected");
  ASSERT_NE(corrected, nullptr);
  EXPECT_LT(corrected->delta, 1'000U) << "the controller was reset; it did not correct "
                                      << corrected->delta << " errors in a second";
}

// statistics Window and timeline PhaseMark: a phase lasts "from start_ns until
// the next mark", and a window is [start_ns, end_ns). Taking each phase's
// window as exactly that leaves the counter's advance across the mark (from
// the last reading before it to the reading on it) in neither phase, and the
// phases' deltas sum to less than the run's. (F13: an ECC delta per phase.)
TEST(Disagreement, Statistics_SignatureHpp_PhasesTakenAtTheirMarksSumToTheRun) {
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
  const auto whole = stack.summarizer.summarize(stack.timeline(), {first, last + 1}).value();
  const std::uint64_t run = counter_of(whole, "edac.corrected")->delta;
  std::uint64_t phases = 0;
  const auto marks = stack.timeline().phases();
  const statistics::Window windows[] = {{first, marks[0].start_ns}, {marks[0].start_ns, last + 1}};
  for (const statistics::Window& window : windows) {
    phases +=
        counter_of(stack.summarizer.summarize(stack.timeline(), window).value(), "edac.corrected")
            ->delta;
  }
  EXPECT_EQ(phases, run) << "the advance across the boundary belongs to a phase";
}

// results result.hpp, Verdict::kIncomplete -- "something was not checked" --
// and statistics signature.hpp, CounterSummary::delta -- "across a gap the
// counter may have reset, and an advance that cannot be known is not guessed".
// An EDAC counter that could not be read for part of a run has an unknown
// advance for that part, and the run that verified everything else is still
// "clean": the corrected errors may have happened in the gap, and nothing in
// the verdict's four rules reads `unavailable_samples`.
TEST(Disagreement, Results_ResultHpp_ACounterBlindForPartOfTheRunIsNotAClaimOfCleanliness) {
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
  EXPECT_NE(result.verdict, results::Verdict::kClean)
      << "hardware errors may have been corrected while the counter could not be read";
}

// telemetry/source.hpp: probe() "is called once, before any sample()"; and
// telemetry/capability/capability.hpp: the sources "are probed here and
// nowhere else ... and the sampler that follows uses the same objects". The
// sampler's own probe() (sampler.hpp) calls each source's probe() again, so
// the two components cannot both be followed: a controller that reports the
// capabilities with one and samples with the other probes every source twice.
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

TEST(Disagreement, Capability_SourceHpp_ASourceIsProbedOnceWhoeverReportsItsCapabilities) {
  auto source = std::make_unique<CountingSource>();
  CountingSource* counting = source.get();
  telemetry::Source* const raw[] = {counting};
  static_cast<void>(telemetry::probe_capabilities(raw));
  std::vector<std::unique_ptr<telemetry::Source>> sources;
  sources.push_back(std::move(source));
  telemetry::BoundedTimeline timeline(100);
  const auto sampler = telemetry::make_sampler(std::move(sources), timeline);
  ASSERT_TRUE(sampler->probe().has_value());
  EXPECT_EQ(counting->probes, 1);
}

}  // namespace
}  // namespace loadforge::m3
