// SPDX-License-Identifier: GPL-3.0-or-later
//
// T8, the real-against-real tier of ADR 0003 (M3 exit criteria 1 to 4): the
// telemetry sources, sampler, timeline, statistics, result and both writers,
// as their own streams built them, run together over the kernel's files served
// as fixtures. Nothing here is a mock of anything that was built.

#include <gtest/gtest.h>
#include <linux/perf_event.h>

#include <cstdint>
#include <string>
#include <variant>

#include "stack.hpp"

namespace loadforge::m3 {
namespace {

using statistics::CounterSummary;
using statistics::GaugeSummary;

void script_counters(Machine& machine, std::uint64_t cycles_per_round) {
  std::vector<testing::FakeSyscalls::PerfStep> steps;
  for (std::uint64_t round = 0; round < 20; ++round) {
    steps.push_back({platform::PerfCount{round * cycles_per_round, 1000, 1000}, 0});
  }
  machine.syscalls().script_perf_reads(kPerfHardware, PERF_COUNT_HW_CPU_CYCLES, steps);
}

TEST(FullSignatureTest, EverySourceContributesToOneSignatureFromFixturesAlone) {
  Stack stack;
  stack.machine.install_everything();
  script_counters(stack.machine, 1'000'000);
  stack.start();
  ASSERT_EQ(count(stack.capabilities()).metrics, 27U);
  ASSERT_EQ(count(stack.capabilities()).present, 27U);

  std::uint64_t energy = 123'456'789'012;
  for (int round = 0; round < 5; ++round) {
    stack.machine.set("/sys/class/hwmon/hwmon0/temp1_input",
                      std::to_string(45'000 + (1000 * round)));
    stack.machine.set("/sys/class/powercap/intel-rapl:0/energy_uj", std::to_string(energy));
    energy += 5'000'000;
    stack.machine.set("/sys/devices/system/edac/mc/mc1/ce_count", round < 3 ? "0" : "2");
    stack.round();
  }
  const statistics::LoadSignature signature = stack.signature();
  EXPECT_EQ(signature.metrics.size(), 27U) << "one summary per series, none for an absent one";

  const auto* package =
      std::get_if<GaugeSummary>(&Stack::find(signature, "thermal.package")->value);
  ASSERT_NE(package, nullptr);
  EXPECT_EQ(package->samples, 5U);
  EXPECT_DOUBLE_EQ(package->min, 45.0);
  EXPECT_DOUBLE_EQ(package->max, 49.0);
  EXPECT_DOUBLE_EQ(package->mean, 47.0);
  EXPECT_DOUBLE_EQ(*package->slope_per_second, 1.0) << "a degree a second";

  const auto* cpu1 = std::get_if<GaugeSummary>(&Stack::find(signature, "frequency.cpu", 1)->value);
  ASSERT_NE(cpu1, nullptr);
  EXPECT_DOUBLE_EQ(cpu1->mean, 2'600'000'000.0) << "kHz in the file, hertz in the signature";

  const auto* fan = std::get_if<GaugeSummary>(&Stack::find(signature, "fan.speed")->value);
  ASSERT_NE(fan, nullptr);
  EXPECT_DOUBLE_EQ(fan->mean, 1200.0);

  const auto* available =
      std::get_if<GaugeSummary>(&Stack::find(signature, "memory.available")->value);
  ASSERT_NE(available, nullptr);
  EXPECT_EQ(available->samples, 5U);
  EXPECT_GT(available->mean, 1e6) << "kB in the file, bytes in the signature";

  const auto* joules =
      std::get_if<CounterSummary>(&Stack::find(signature, "power.package", 0)->value);
  ASSERT_NE(joules, nullptr);
  EXPECT_EQ(joules->delta, 20'000'000U) << "four steps of 5 J, in microjoules";

  const auto* cycles =
      std::get_if<CounterSummary>(&Stack::find(signature, "counters.cycles")->value);
  ASSERT_NE(cycles, nullptr);
  EXPECT_EQ(cycles->delta, 4'000'000U);

  const auto* errors =
      std::get_if<CounterSummary>(&Stack::find(signature, "edac.corrected", 1)->value);
  ASSERT_NE(errors, nullptr);
  EXPECT_EQ(errors->delta, 2U);
  const auto* none =
      std::get_if<CounterSummary>(&Stack::find(signature, "edac.corrected", 0)->value);
  ASSERT_NE(none, nullptr);
  EXPECT_EQ(none->delta, 0U);

  // And the whole of it reaches both renderings.
  const results::RunResult result = stack.result();
  const std::string json = Stack::json(result);
  const std::string csv = Stack::csv(result);
  for (const char* name : {"thermal.package", "thermal.core", "thermal.zone", "fan.speed",
                           "frequency.cpu", "frequency.busy_ticks", "frequency.total_ticks",
                           "memory.total", "memory.available", "power.package", "counters.cycles",
                           "counters.context_switches", "edac.corrected", "edac.uncorrected"}) {
    EXPECT_NE(json.find(std::string{"\"name\": \""} + name + "\""), std::string::npos) << name;
    EXPECT_NE(csv.find(name), std::string::npos) << name;
  }
  EXPECT_NE(json.find("\"schema_version\": 2"), std::string::npos);
  EXPECT_NE(json.find("\"delta\": 2,"), std::string::npos) << "the corrected errors, in the JSON";
}

}  // namespace
}  // namespace loadforge::m3
