// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the stack's result says (M3 exit criteria 3, 5 and 6, and F13, F17):
// the verdict that hardware errors imply, the per-phase attribution of a
// counter, the observers and the verification duty cycle, in both writers.

#include <gtest/gtest.h>

#include <cerrno>
#include <string>
#include <variant>

#include "controller/results/aggregate.hpp"
#include "stack.hpp"

namespace loadforge::m3 {
namespace {

using statistics::CounterSummary;

/// A result of the stack that verified everything and in which every worker
/// finished: the verdict then depends on the hardware alone.
results::RunResult verified(const Stack& stack) {
  results::RunResult result = stack.result();
  result.workloads[0].tally = verification::Tally{100, 10, 10, 0, 0, 10, 0};
  result.workloads[0].duty = verification::DutyCycle{10, 100};
  result.workers[0].fate = controller::WorkerFate::kCompleted;
  results::seal(result);
  return result;
}

TEST(ResultTest, ARunThatVerifiedEverythingAndSawNoHardwareErrorIsClean) {
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  stack.rounds(3);
  EXPECT_EQ(verified(stack).verdict, results::Verdict::kClean);
}

TEST(ResultTest, ACorrectedErrorCountedByTheKernelMakesTheRunNotClean) {
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  stack.round();
  stack.machine.set("/sys/devices/system/edac/mc/mc1/ce_count", "1");
  stack.rounds(2);
  EXPECT_EQ(verified(stack).verdict, results::Verdict::kCorrectedErrors);
}

TEST(ResultTest, AnUncorrectedErrorIsAFindingWhateverElseWasVerified) {
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  stack.round();
  stack.machine.set("/sys/devices/system/edac/mc/mc0/ue_count", "1");
  stack.machine.set("/sys/devices/system/edac/mc/mc1/ce_count", "9");
  stack.rounds(2);
  EXPECT_EQ(verified(stack).verdict, results::Verdict::kFindings);
}

TEST(ResultTest, ARunWithoutEdacSaysSoInItsCapabilitiesAndIsStillJudgedOnWhatWasRead) {
  Stack stack;
  stack.machine.install_everything_but({"edac"});
  stack.start();
  stack.rounds(3);
  const results::RunResult result = verified(stack);
  EXPECT_EQ(result.verdict, results::Verdict::kClean)
      << "the verdict contract: absent counters are not errors";
  EXPECT_NE(Stack::json(result).find("\"source\": \"edac\""), std::string::npos)
      << "and the capability report is where it is said that EDAC was not read";
}

TEST(ResultTest, TheDutyCycleAndTheObserversAreRecordedBesideTheSignature) {
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  stack.rounds(3);
  const results::RunResult result = verified(stack);
  for (const std::string& text : {Stack::json(result), Stack::csv(result)}) {
    EXPECT_NE(text.find("oracle_period"), std::string::npos);
    EXPECT_NE(text.find("integrity_period"), std::string::npos);
    EXPECT_NE(text.find("sample_interval_ns"), std::string::npos);
    EXPECT_NE(text.find("controller_overhead"), std::string::npos);
  }
  EXPECT_NE(Stack::json(result).find("\"oracle_period\": 10"), std::string::npos);
  EXPECT_NE(Stack::json(result).find("\"integrity_period\": 100"), std::string::npos);
  EXPECT_NE(Stack::json(result).find("\"sample_interval_ns\": 1000000000"), std::string::npos);
}

TEST(ResultTest, TheEveryAbsentMetricIsInTheCapabilitiesWithItsRemedyAndNowhereElseAsANumber) {
  Stack stack;
  stack.machine.install_everything_but({"fan", "thermal"});
  stack.machine.syscalls().fail_every_perf_open(ENOENT);
  stack.start();
  stack.rounds(2);
  const results::RunResult result = stack.result();
  const auto counted = count(stack.capabilities());
  EXPECT_EQ(counted.metrics - counted.present, 1U + 1U + 5U)
      << "fan, thermal.package (the one metric an absent thermal source looks for) and five "
         "counters";
  // Capabilities carry every metric, present or not; the signature only
  // the ones that were read.
  EXPECT_EQ(result.signature.metrics.size(), counted.present);
  const std::string json = Stack::json(result);
  const std::size_t capabilities = json.find("\"capabilities\"");
  const std::size_t signature = json.find("\"signature\"");
  ASSERT_NE(capabilities, std::string::npos);
  ASSERT_NE(signature, std::string::npos);
  for (const telemetry::SourceStatus& source : stack.capabilities().sources) {
    for (const telemetry::MetricStatus& status : source.metrics) {
      if (status.unavailable.has_value()) {
        const std::size_t at = json.find(status.unavailable->remediation);
        EXPECT_GT(at, capabilities) << status.metric.name;
        EXPECT_LT(at, signature) << status.metric.name;
        EXPECT_EQ(Stack::find(result.signature, status.metric.name,
                              status.metric.scope_index == 0 ? 0 : 1),
                  nullptr)
            << status.metric.name << " has a summary though it was absent";
      }
    }
  }
}

// ---- F13: a counter per phase ------------------------------------------------------

TEST(PhaseTest, EveryAdvanceOfACounterBelongsToExactlyOnePhase) {
  Stack stack;
  stack.machine.install_everything();
  stack.start();
  const auto errors = [&](const char* mc0, const char* mc1) {
    stack.machine.set("/sys/devices/system/edac/mc/mc0/ce_count", mc0);
    stack.machine.set("/sys/devices/system/edac/mc/mc1/ce_count", mc1);
    stack.round();
  };
  ASSERT_TRUE(stack.rig->timeline.mark_phase({stack.now, "run"}).has_value());
  errors("0", "0");
  errors("1", "0");
  ASSERT_TRUE(stack.rig->timeline.mark_phase({stack.now, "stop"}).has_value());
  errors("4", "2");  // on the boundary: the advance since the last reading is the run's
  errors("5", "2");
  const std::int64_t last = stack.now - kSecond;

  const auto deltas =
      cli::counter_by_phase(stack.timeline(), stack.summarizer, "edac.corrected", last);
  std::uint64_t in_phases = 0;
  for (const cli::PhaseDelta& delta : deltas) {
    in_phases += delta.delta;
  }
  const auto whole = stack.signature();
  std::uint64_t in_run = 0;
  for (std::size_t mc = 0; mc < 2; ++mc) {
    in_run += std::get<CounterSummary>(Stack::find(whole, "edac.corrected", mc)->value).delta;
  }
  EXPECT_EQ(in_run, 7U);
  EXPECT_EQ(in_phases, in_run) << "per-phase deltas sum to the run's";

  ASSERT_EQ(deltas.size(), 3U);
  EXPECT_EQ(deltas[0].phase, "run");
  EXPECT_EQ(deltas[0].metric, "edac.corrected memory controller 0");
  EXPECT_EQ(deltas[0].delta, 4U) << "1 in the second, and the 3 that arrived by the mark";
  EXPECT_EQ(deltas[1].phase, "run");
  EXPECT_EQ(deltas[1].metric, "edac.corrected memory controller 1");
  EXPECT_EQ(deltas[1].delta, 2U);
  EXPECT_EQ(deltas[2].phase, "stop");
  EXPECT_EQ(deltas[2].metric, "edac.corrected memory controller 0");
  EXPECT_EQ(deltas[2].delta, 1U) << "what the stop phase itself counted";
}

}  // namespace
}  // namespace loadforge::m3
