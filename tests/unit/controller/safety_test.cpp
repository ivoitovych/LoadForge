// SPDX-License-Identifier: GPL-3.0-or-later
//
// The escalation ladder as a function of time, and the limits a run states
// with what this build does about each.

#include "controller/safety.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "core/byte_size.hpp"
#include "core/duration.hpp"

namespace loadforge::controller {
namespace {

using loadforge::config::Config;
using loadforge::config::MemoryBudget;
using loadforge::config::Workload;
using loadforge::core::ByteSize;

constexpr std::int64_t kMs = 1'000'000;
constexpr std::uint64_t kMiB = std::uint64_t{1} << 20U;

Config base_config() {
  Config config;
  config.run.mode = config::Mode::kStress;
  config.run.duration = core::Duration{30'000};
  config.workloads.push_back(Workload{"null", config::Threads::all(), true, {}});
  return config;
}

TEST(EscalationTest, EveryStepHasWordsAndEachGraceIsStrict) {
  EXPECT_EQ(describe(StopStep::kNone), "running");
  EXPECT_EQ(describe(StopStep::kRequested), "stop requested");
  EXPECT_EQ(describe(StopStep::kTerminated), "SIGTERM sent");
  EXPECT_EQ(describe(StopStep::kKilled), "SIGKILL sent");
  EXPECT_EQ(describe(static_cast<StopStep>(200)), "unrecognised stop step");

  const Grace grace{100 * kMs, 50 * kMs};
  EXPECT_EQ(escalation_due(StopStep::kNone, 0, 1'000'000 * kMs, grace), std::nullopt)
      << "nothing was asked; nothing is due";
  EXPECT_EQ(escalation_due(StopStep::kRequested, 1000 * kMs, 1099 * kMs, grace), std::nullopt);
  EXPECT_EQ(escalation_due(StopStep::kRequested, 1000 * kMs, 1100 * kMs, grace), SIGTERM);
  EXPECT_EQ(escalation_due(StopStep::kTerminated, 1100 * kMs, 1149 * kMs, grace), std::nullopt);
  EXPECT_EQ(escalation_due(StopStep::kTerminated, 1100 * kMs, 1150 * kMs, grace), SIGKILL);
  EXPECT_EQ(escalation_due(StopStep::kKilled, 0, 1'000'000 * kMs, grace), std::nullopt)
      << "nothing comes after SIGKILL";

  EXPECT_EQ(step_after(SIGTERM), StopStep::kTerminated);
  EXPECT_EQ(step_after(SIGKILL), StopStep::kKilled);

  EXPECT_EQ(grace, (Grace{100 * kMs, 50 * kMs}));
  EXPECT_NE(grace, (Grace{1, 50 * kMs}));
  EXPECT_NE(grace, (Grace{100 * kMs, 1}));
}

TEST(LimitsTest, TheDurationIsAlwaysEnforcedAndTheOthersSayWhatTheyAre) {
  const Config plain = base_config();
  EXPECT_EQ(limit_statuses(plain, std::nullopt, ""),
            (std::vector<LimitStatus>{LimitStatus{"duration", true, "30s"}}));

  Config with_memory = plain;
  with_memory.limits.memory = ByteSize::bytes(512 * kMiB);
  EXPECT_EQ(
      limit_statuses(with_memory, std::nullopt, ""),
      (std::vector<LimitStatus>{LimitStatus{"duration", true, "30s"},
                                LimitStatus{"memory", false, "no memory budget could be read"}}));
  // The reader's reason, when it gave one, travels with the NOT ENFORCED: a
  // user told only that no budget could be read has nowhere to look.
  EXPECT_EQ(limit_statuses(with_memory, std::nullopt, "/proc/meminfo: not permitted (x)")[1],
            (LimitStatus{"memory", false,
                         "no memory budget could be read: /proc/meminfo: not permitted (x)"}));
  const MemoryBudget budget{1024 * kMiB, std::optional<std::uint64_t>{768 * kMiB}};
  EXPECT_EQ(limit_statuses(with_memory, budget, "")[1],
            (LimitStatus{"memory", true, "checked at start against a ceiling of 768MiB"}));

  Config workload_memory = plain;
  workload_memory.workloads[0].memory = ByteSize::bytes(kMiB);
  EXPECT_EQ(limit_statuses(workload_memory, std::nullopt, "").size(), 2U)
      << "a workload footprint is a memory limit to state";

  Config with_temperature = plain;
  with_temperature.limits.max_temperature_c = 95;
  EXPECT_EQ(limit_statuses(with_temperature, std::nullopt, ""),
            (std::vector<LimitStatus>{
                LimitStatus{"duration", true, "30s"},
                LimitStatus{"temperature", false,
                            "not enforced until telemetry lands (M3); configured 95 C"}}));

  const MemoryBudget sample{1, std::optional<std::uint64_t>{2}};
  // Default-initialised and then changed, so the member initialisers run as
  // code here rather than being folded into a constant.
  MemoryBudget empty;
  EXPECT_EQ(empty.available_bytes, 0U);
  EXPECT_FALSE(empty.cgroup_limit_bytes.has_value());
  empty.available_bytes += 1;
  empty.cgroup_limit_bytes = 2;
  EXPECT_EQ(empty, sample);
  EXPECT_EQ(sample, (MemoryBudget{1, std::optional<std::uint64_t>{2}}));
  EXPECT_NE(sample, (MemoryBudget{9, std::optional<std::uint64_t>{2}}));
  EXPECT_NE(sample, (MemoryBudget{1, std::nullopt}));

  const LimitStatus status{"a", true, "b"};
  EXPECT_EQ(status, (LimitStatus{"a", true, "b"}));
  EXPECT_NE(status, (LimitStatus{"x", true, "b"}));
  EXPECT_NE(status, (LimitStatus{"a", false, "b"}));
  EXPECT_NE(status, (LimitStatus{"a", true, "x"}));
}

TEST(LimitsTest, MemoryIsCheckedAgainstTheBudgetBeforeAnyWorkerStarts) {
  Config config = base_config();
  config.workloads[0].memory = ByteSize::bytes(300 * kMiB);
  config.workloads.push_back(Workload{"second", config::Threads::all(), true,
                                      std::optional<ByteSize>{ByteSize::bytes(300 * kMiB)}});
  config.workloads.push_back(Workload{"off", config::Threads::all(), false,
                                      std::optional<ByteSize>{ByteSize::bytes(5000 * kMiB)}});
  EXPECT_TRUE(check_memory(config, std::nullopt).has_value()) << "no budget: nothing to check";

  const MemoryBudget roomy{1024 * kMiB, std::nullopt};
  EXPECT_TRUE(check_memory(config, roomy).has_value())
      << "600 MiB within 1024 MiB; disabled skipped";

  const MemoryBudget tight{500 * kMiB, std::nullopt};
  auto refused = check_memory(config, tight);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error(), "the workloads' memory adds up to 600MiB, past the ceiling of 500MiB");

  Config absolute_too_large = config;
  absolute_too_large.workloads[1].memory = ByteSize::bytes(2000 * kMiB);
  auto oversize = check_memory(absolute_too_large, roomy);
  ASSERT_FALSE(oversize.has_value());
  EXPECT_NE(oversize.error().find("workload.second"), std::string::npos) << oversize.error();

  Config limited = config;
  limited.limits.memory = ByteSize::bytes(400 * kMiB);
  auto past_limit = check_memory(limited, roomy);
  ASSERT_FALSE(past_limit.has_value());
  EXPECT_EQ(past_limit.error(),
            "the workloads' memory adds up to 600MiB, past limits.memory of 400MiB");
  limited.limits.memory = ByteSize::bytes(800 * kMiB);
  EXPECT_TRUE(check_memory(limited, roomy).has_value()) << "600 MiB within limits.memory";
  limited.limits.memory = ByteSize::bytes(600 * kMiB);
  EXPECT_TRUE(check_memory(limited, roomy).has_value()) << "600 MiB fills limits.memory exactly";

  Config one_without_memory = config;
  one_without_memory.workloads.push_back(
      Workload{"no footprint", config::Threads::all(), true, {}});
  EXPECT_TRUE(check_memory(one_without_memory, roomy).has_value())
      << "an enabled workload with no footprint adds nothing";

  Config limit_too_large = config;
  limit_too_large.limits.memory = ByteSize::bytes(4000 * kMiB);
  auto bad_limit = check_memory(limit_too_large, roomy);
  ASSERT_FALSE(bad_limit.has_value());
  EXPECT_NE(bad_limit.error().find("limits.memory"), std::string::npos) << bad_limit.error();

  Config shares = config;
  shares.workloads[0].memory = ByteSize::percent(50).value();
  shares.workloads[1].memory = ByteSize::percent(50).value();
  EXPECT_TRUE(check_memory(shares, roomy).has_value()) << "two halves fill the ceiling exactly";
}

}  // namespace
}  // namespace loadforge::controller
