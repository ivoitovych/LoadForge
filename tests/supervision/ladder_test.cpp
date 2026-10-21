// SPDX-License-Identifier: GPL-3.0-or-later
//
// T7: every rung of the isolation ladder against really re-run workers. The
// real controller runs two real worker processes (the faulty worker, which is
// the shipped worker around a compression workload that corrupts its output
// the way LOADFORGE_FAULT says), collects what they send, and then the real
// re-runner executes the same program once per rung, pinned where the rung
// says. N >= 2 throughout.

#include "verification/isolation/ladder.hpp"

#include <gtest/gtest.h>
#include <sched.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "config/config.hpp"
#include "controller/isolate.hpp"
#include "controller/orchestration.hpp"
#include "controller/rerun.hpp"
#include "main/cli.hpp"
#include "main/outcome.hpp"
#include "platform/affinity.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/snapshot.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::controller {
namespace {

constexpr std::int64_t kMs = 1'000'000;

class Quiet final : public Reporter {
 public:
  void run_starting(const RunPlan& /*plan*/) override {}
  void event(std::int64_t /*elapsed_ns*/, std::string_view /*line*/) override {}
  void run_finished(const RunReport& /*report*/) override {}
  void isolation_finished(const RunReport& /*report*/) override {}
};

struct Scenario {
  RunReport report;
  config::Config config;
  IsolationSummary summary;
  std::vector<std::uint32_t> cpus;
};

/// Two workers of `threads` threads each on `cpus`, run for a moment with the
/// fault the environment names, then isolated.
std::optional<Scenario> run_scenario(const std::string& fault, std::uint32_t threads,
                                     std::size_t cpu_count,
                                     const std::string& workload = "compression") {
  platform::RealSyscalls syscalls;
  platform::Affinity affinity(syscalls);
  auto permitted = affinity.permitted();
  if (!permitted.has_value()) {
    return std::nullopt;
  }
  std::vector<std::uint32_t> allowed = platform::ids_of(permitted.value());
  if (allowed.size() < 2) {
    return std::nullopt;
  }
  allowed.resize(cpu_count);
  // "core" names the first CPU the workers may use: worker 0 runs there.
  const std::string spec = fault == "core" ? "core:" + std::to_string(allowed.front()) : fault;
  ::setenv("LOADFORGE_FAULT", spec.c_str(), 1);

  platform::FileSystem filesystem(syscalls);
  auto machine = topology::take_snapshot(filesystem);
  if (!machine.has_value()) {
    return std::nullopt;
  }
  const workloads::BuiltinRegistry registry;
  RunOptions options;
  options.executable = LOADFORGE_FAULTY_WORKER;
  options.interval_ns = 20 * kMs;
  options.stale_after_ns = 10000 * kMs;
  options.grace = Grace{6000 * kMs, 1000 * kMs};
  options.topology = &machine.value();
  options.verification.registry = &registry;
  // A dense_linear unit costs the cube of its side: small enough that an
  // instrumented build still finishes a unit well inside the stop grace.
  options.verification.footprint_bytes = std::uint64_t{workload == "dense_linear" ? 2U : 4U} << 20U;
  options.verification.line_bytes = 64;
  options.verification.cpus = allowed;

  config::Config config;
  config.run.mode = config::Mode::kStress;
  config.run.duration = core::Duration{1500};
  for (int worker = 0; worker < 2; ++worker) {
    config.workloads.push_back(
        config::Workload{workload, config::Threads::count(threads), true, {}});
  }
  Quiet reporter;
  Controller controller(syscalls, reporter);
  auto ran = controller.run(config, options);
  if (!ran.has_value()) {
    ADD_FAILURE() << describe(ran.error());
    return std::nullopt;
  }
  Scenario scenario;
  scenario.report = ran.value();
  scenario.config = config;
  scenario.cpus = allowed;
  verification::isolation::LadderIsolator isolator;
  RealRerunner rerunner(syscalls, LOADFORGE_FAULTY_WORKER, RerunOptions{});
  scenario.summary = isolate_findings(scenario.report, &machine.value(), isolator, rerunner, 2);
  return scenario;
}

/// The first isolated finding of the worker in `slot`, if any.
const verification::IsolationReport* isolated(const Scenario& scenario, std::size_t slot) {
  for (const FindingReport& finding : scenario.report.workers[slot].findings) {
    if (finding.isolation.has_value()) {
      return &*finding.isolation;
    }
  }
  return nullptr;
}

TEST(IsolationLadder, ACorruptionThatRecursEverywhereIsNotCoreLocal) {
  const auto scenario = run_scenario("all", 1, 2);
  if (!scenario.has_value()) {
    GTEST_SKIP() << "needs two CPUs this process may use";
  }
  for (std::size_t slot = 0; slot < 2; ++slot) {
    const auto* report = isolated(*scenario, slot);
    ASSERT_NE(report, nullptr) << "worker " << slot;
    EXPECT_EQ(report->finding, verification::Finding::kNotCoreLocal);
    EXPECT_EQ(report->confidence, verification::Confidence::kHigh);
    EXPECT_EQ(report->steps.size(), 2U) << "same core, another core; single thread is not repeated";
  }
}

TEST(IsolationLadder, ACorruptionOnOneCoreIsCoreLocalAndOnlyThatWorkerFindsIt) {
  const auto scenario = run_scenario("core", 1, 2);
  if (!scenario.has_value()) {
    GTEST_SKIP() << "needs two CPUs this process may use";
  }
  const auto* report = isolated(*scenario, 0);
  ASSERT_NE(report, nullptr);
  EXPECT_EQ(report->finding, verification::Finding::kCoreLocal);
  EXPECT_EQ(report->answers.same_core, verification::Answer::kYes);
  EXPECT_EQ(report->answers.other_core, verification::Answer::kNo);
  EXPECT_EQ(scenario->report.workers[1].wrong_results, 0U)
      << "the other worker runs on a core that is fine";
}

TEST(IsolationLadder, ACorruptionThatDoesNotRecurIsATransient) {
  const auto scenario = run_scenario("transient", 1, 2);
  if (!scenario.has_value()) {
    GTEST_SKIP() << "needs two CPUs this process may use";
  }
  for (std::size_t slot = 0; slot < 2; ++slot) {
    const auto* report = isolated(*scenario, slot);
    ASSERT_NE(report, nullptr) << "worker " << slot;
    EXPECT_EQ(report->finding, verification::Finding::kNonReproducibleTransient);
    EXPECT_EQ(report->confidence, verification::Confidence::kLow);
    EXPECT_EQ(report->steps.size(), 1U) << "a clean first rung stops the ladder";
  }
}

TEST(IsolationLadder, ACorruptionOfSeveralThreadsOnlyIsConcurrencyDependent) {
  // One CPU for both workers' two threads each: the record names that CPU,
  // and the single-thread rung is the one that runs clean.
  const auto scenario = run_scenario("threads", 2, 1);
  if (!scenario.has_value()) {
    GTEST_SKIP() << "needs two CPUs this process may use";
  }
  for (std::size_t slot = 0; slot < 2; ++slot) {
    const auto* report = isolated(*scenario, slot);
    ASSERT_NE(report, nullptr) << "worker " << slot;
    EXPECT_EQ(report->finding, verification::Finding::kConcurrencyDependent);
    EXPECT_EQ(report->answers.single_thread, verification::Answer::kNo);
  }
}

TEST(IsolationLadder, AnInputThatChangesBetweenGenerationsIsReportedOnTheInputSideNotIsolated) {
  const auto scenario = run_scenario("input", 1, 2);
  if (!scenario.has_value()) {
    GTEST_SKIP() << "needs two CPUs this process may use";
  }
  for (std::size_t slot = 0; slot < 2; ++slot) {
    const WorkerReport& worker = scenario->report.workers[slot];
    ASSERT_GT(worker.wrong_results, 0U) << "worker " << slot;
    const auto* found = std::get_if<verification::ExactBitCorruption>(&worker.findings[0].record);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->side, verification::Side::kInput);
    ASSERT_TRUE(found->provenance.input_integrity.has_value());
    EXPECT_EQ(*found->provenance.input_integrity,
              verification::InputIntegrity::kCorruptBeforeConsumption);
  }
}

TEST(IsolationLadder, AnOracleThatJudgesNothingLeavesTheRunNotVerifiedAndIsolatesNothing) {
  const auto scenario = run_scenario("unchecked", 1, 2);
  if (!scenario.has_value()) {
    GTEST_SKIP() << "needs two CPUs this process may use";
  }
  for (std::size_t slot = 0; slot < 2; ++slot) {
    const WorkerReport& worker = scenario->report.workers[slot];
    EXPECT_GT(worker.not_performed, 0U) << "worker " << slot;
    EXPECT_EQ(worker.wrong_results, 0U) << "worker " << slot;
    EXPECT_EQ(isolated(*scenario, slot), nullptr) << "nothing was found to isolate";
  }
  EXPECT_EQ(cli::exit_code_for(scenario->report, scenario->config), cli::kExitNotVerified);
}

TEST(IsolationLadder, ABoundedCorruptionIsANumericalFailureWithNoBitPatternToIsolate) {
  const auto scenario = run_scenario("dense", 1, 2, "dense_linear");
  if (!scenario.has_value()) {
    GTEST_SKIP() << "needs two CPUs this process may use";
  }
  for (std::size_t slot = 0; slot < 2; ++slot) {
    const WorkerReport& worker = scenario->report.workers[slot];
    ASSERT_GT(worker.wrong_results, 0U) << "worker " << slot;
    EXPECT_NE(std::get_if<verification::NumericalVerificationFailure>(&worker.findings[0].record),
              nullptr);
  }
}

TEST(IsolationLadder, TheCommandLineReportsAWrongResultIsolatesItAndExitsWithTheFindingsCode) {
  platform::RealSyscalls syscalls;
  platform::Affinity affinity(syscalls);
  auto permitted = affinity.permitted();
  ASSERT_TRUE(permitted.has_value());
  if (platform::ids_of(permitted.value()).size() < 2) {
    GTEST_SKIP() << "needs two CPUs this process may use";
  }
  ::setenv("LOADFORGE_FAULT", "all", 1);
  platform::FileSystem filesystem(syscalls);
  auto machine = topology::take_snapshot(filesystem);
  ASSERT_TRUE(machine.has_value());
  config::Config config;
  config.run.mode = config::Mode::kStress;
  config.run.duration = core::Duration{1500};
  for (int worker = 0; worker < 2; ++worker) {
    config.workloads.push_back(
        config::Workload{"compression", config::Threads::count(1), true, {}});
  }
  cli::StressOptions options;
  options.capabilities = false;
  options.footprint_bytes = std::uint64_t{4} << 20U;
  options.isolation_limit = 1;
  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(cli::run_stress(syscalls, config, LOADFORGE_FAULTY_WORKER, machine.value(), options,
                            out, err),
            cli::kExitWrongResult)
      << err.str();
  const std::string text = out.str();
  EXPECT_NE(text.find("ExactBitCorruption"), std::string::npos) << text;
  EXPECT_NE(text.find("worker 0 isolation of ExactBitCorruption:"), std::string::npos) << text;
  EXPECT_NE(text.find("      isolation: same core yes, another core yes, single thread yes\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("not isolated: past the limit of 1 isolations"), std::string::npos) << text;
}

}  // namespace
}  // namespace loadforge::controller
