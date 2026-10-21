// SPDX-License-Identifier: GPL-3.0-or-later
//
// T8: the pipeline end to end with nothing mocked: the shipped binary as the
// worker, the real controller, the real registries, the real runner, oracle
// and codec, the real kernel. Every workload this build has is run, so the
// places where a workload and its oracle (written apart) read a configuration
// differently show up here and nowhere else.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "controller/orchestration.hpp"
#include "main/outcome.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/snapshot.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::controller {
namespace {

constexpr std::int64_t kMs = 1'000'000;

class Quiet final : public Reporter {
 public:
  void run_starting(const RunPlan& plan) override { plans.push_back(plan); }
  void event(std::int64_t /*elapsed_ns*/, std::string_view /*line*/) override {}
  void run_finished(const RunReport& /*report*/) override {}
  void isolation_finished(const RunReport& /*report*/) override {}
  std::vector<RunPlan> plans;
};

TEST(Pipeline, EveryWorkloadOfTheBuildRunsCleanAndIsCheckedByItsOracle) {
  const workloads::BuiltinRegistry registry;
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  auto machine = topology::take_snapshot(filesystem);
  ASSERT_TRUE(machine.has_value());
  for (const std::string& name : registry.names()) {
    RunOptions options;
    options.executable = LOADFORGE_BINARY;
    options.interval_ns = 20 * kMs;
    options.stale_after_ns = 10000 * kMs;
    options.grace = Grace{6000 * kMs, 1000 * kMs};
    options.topology = &machine.value();
    options.verification.registry = &registry;
    options.verification.footprint_bytes = std::uint64_t{3} << 20U;
    options.verification.line_bytes = 64;
    config::Config config;
    config.run.mode = config::Mode::kStress;
    config.run.duration = core::Duration{1500};
    for (int worker = 0; worker < 2; ++worker) {
      config.workloads.push_back(config::Workload{name, config::Threads::count(1), true, {}});
    }
    Quiet reporter;
    Controller controller(syscalls, reporter);
    auto ran = controller.run(config, options);
    ASSERT_TRUE(ran.has_value()) << name << ": " << describe(ran.error());
    const RunReport& report = ran.value();
    ASSERT_EQ(report.workers.size(), 2U) << name;
    for (const WorkerReport& worker : report.workers) {
      EXPECT_EQ(worker.fate, WorkerFate::kCompleted) << name << ": " << worker.termination;
      EXPECT_TRUE(worker.tally_seen) << name;
      EXPECT_GT(worker.tally.units_computed, 0U) << name;
      EXPECT_GT(worker.tally.oracle_blocks, 0U) << name;
      EXPECT_GT(worker.tally.integrity_checked, 0U) << name;
      EXPECT_EQ(worker.wrong_results, 0U) << name;
      EXPECT_EQ(worker.not_performed, 0U) << name;
    }
    EXPECT_EQ(cli::exit_code_for(report, config), 0) << name;
  }
}

}  // namespace
}  // namespace loadforge::controller
