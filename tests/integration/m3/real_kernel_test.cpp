// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR 0003, rule 4: the real stack against the real kernel. The same sources,
// sampler, timeline, statistics and writers as the fixture tests, over this
// machine's own /proc and /sys, with whatever it really offers. A virtual
// machine offers little (no sensor, no fan, no cpufreq, often no PMU), and
// that is the point: the stack must come out the other end with a signature
// of what is there and a remedy for what is not, on any machine.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <thread>
#include <variant>

#include "controller/results/fingerprint.hpp"
#include "main/run_result.hpp"
#include "main/telemetry_rig.hpp"
#include "platform/clock.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "stack_real.hpp"

namespace loadforge::m3 {
namespace {

using statistics::CounterSummary;
using statistics::GaugeSummary;

/// Burns the CPU for about `duration`, in the calling thread, so the kernel's
/// own tick counters have something to count.
void burn(std::chrono::milliseconds duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  volatile std::uint64_t sink = 0;
  while (std::chrono::steady_clock::now() < until) {
    sink = sink + 1;
  }
}

TEST(RealKernelTest, TheTimelinesClockIsTheKernelsMonotonicClock) {
  platform::RealSyscalls syscalls;
  platform::Clock clock(syscalls);
  const auto before = clock.now();
  ASSERT_TRUE(before.has_value());
  timespec kernel{};
  ASSERT_EQ(::clock_gettime(CLOCK_MONOTONIC, &kernel), 0);
  const auto after = clock.now();
  ASSERT_TRUE(after.has_value());
  const std::int64_t kernel_ns =
      (static_cast<std::int64_t>(kernel.tv_sec) * 1'000'000'000) + kernel.tv_nsec;
  EXPECT_GE(kernel_ns, before.value().running_ns);
  EXPECT_LE(kernel_ns, after.value().running_ns)
      << "timeline.hpp: every timestamp is CLOCK_MONOTONIC; the heartbeat's clock is running_ns";
}

TEST(RealKernelTest, TheWholeStackRunsOverThisMachinesFiles) {
  RealStack stack;
  stack.start();
  ASSERT_GT(count(stack.capabilities()).metrics, 0U);

  for (const telemetry::SourceStatus& source : stack.capabilities().sources) {
    ASSERT_FALSE(source.metrics.empty())
        << source.source << " reported nothing, not even an absence";
    for (const telemetry::MetricStatus& status : source.metrics) {
      EXPECT_TRUE(telemetry::is_valid_metric_name(status.metric.name)) << status.metric.name;
      if (status.unavailable.has_value()) {
        EXPECT_NE(status.unavailable->kind, topology::Availability::kPresent);
        EXPECT_FALSE(status.unavailable->remediation.empty()) << status.metric.name;
      }
    }
  }

  for (int round = 0; round < 8; ++round) {
    stack.round();
    burn(std::chrono::milliseconds{60});
    std::this_thread::sleep_for(std::chrono::milliseconds{15});
  }
  stack.round();

  const statistics::LoadSignature signature = stack.signature();
  EXPECT_EQ(signature.metrics.size(), count(stack.capabilities()).present)
      << "one summary for every metric that was present, and none for the others";

  // /proc/meminfo and /proc/stat exist on every Linux this runs on.
  const auto* available = Stack::find(signature, "memory.available");
  ASSERT_NE(available, nullptr);
  const auto* memory = std::get_if<GaugeSummary>(&available->value);
  ASSERT_NE(memory, nullptr);
  EXPECT_GT(memory->mean, 1e6);
  EXPECT_EQ(memory->samples, 9U);

  const auto* busy = Stack::find(signature, "frequency.busy_ticks");
  ASSERT_NE(busy, nullptr);
  const auto* ticks = std::get_if<CounterSummary>(&busy->value);
  ASSERT_NE(ticks, nullptr);
  EXPECT_GE(ticks->delta, 20U) << "half a second of burning is at least twenty 10 ms ticks";
  const auto* total = Stack::find(signature, "frequency.total_ticks");
  ASSERT_NE(total, nullptr);
  EXPECT_GE(std::get<CounterSummary>(total->value).delta, ticks->delta);

  // counters.context_switches is not asserted here: on this kernel it reads zero
  // however many switches there are (disagreements_test.cpp, the counters
  // source, and docs/journal/21-integration-m3.md for the evidence).

  // And it all reaches both writers.
  const results::RunResult result = stack.result();
  const std::string json = Stack::json(result);
  EXPECT_NE(json.find("\"memory.available\""), std::string::npos);
  EXPECT_NE(json.find("\"frequency.busy_ticks\""), std::string::npos);
  EXPECT_NE(Stack::csv(result).find("memory.available"), std::string::npos);
}

TEST(RealKernelTest, TheFingerprintIsTheSameTwiceOnOneMachineInEverythingButTheMoment) {
  platform::RealSyscalls syscalls;
  platform::FileSystem files(syscalls);
  const results::FingerprintContext context{results::this_build(), std::nullopt, std::nullopt};
  const auto first = results::read_fingerprint(files, "", context);
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  const auto second = results::read_fingerprint(files, "", context);
  for (const std::string_view field :
       results::differing_fields(first.fingerprint, second.fingerprint)) {
    EXPECT_TRUE(results::is_moment_field(field))
        << field << " differs between two readings a moment apart, and is not a moment's field";
  }
  EXPECT_EQ(first.gaps.size(), second.gaps.size());
  EXPECT_FALSE(first.fingerprint.loadforge_version.empty());
  EXPECT_FALSE(first.fingerprint.compiler.empty());
  EXPECT_TRUE(first.fingerprint.kernel.has_value()) << "/proc/sys/kernel/osrelease exists";
}

}  // namespace
}  // namespace loadforge::m3
