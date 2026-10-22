// SPDX-License-Identifier: GPL-3.0-or-later
//
// T7: every control message against real worker processes (worker/control.hpp).
// Two workers at least in each test, the real binary, a real workload and its
// real oracle: one worker is told something, the other is not, and what the
// controller's report says of each is compared. A worker that was told to
// pause or slow down did less; one that was told to move or to switch went on
// verifying, and found nothing wrong; one that was refused is still running.

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "config/config.hpp"
#include "controller/orchestration.hpp"
#include "controller/report.hpp"
#include "core/duration.hpp"
#include "platform/affinity.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/snapshot.hpp"
#include "workloads/mixed/schedule.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::controller {
namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::uint64_t kFootprint = std::uint64_t{2} << 20U;

class Quiet final : public Reporter {
 public:
  void run_starting(const RunPlan& /*plan*/) override {}
  void event(std::int64_t /*elapsed_ns*/, std::string_view /*line*/) override {}
  void run_finished(const RunReport& /*report*/) override {}
  void isolation_finished(const RunReport& /*report*/) override {}
};

mixed::Step step(std::int64_t at_ms, std::string phase, std::size_t slot,
                 std::vector<worker::Command> commands) {
  mixed::Step made;
  made.at_ns = at_ms * kMs;
  made.phase = std::move(phase);
  made.workload = "raytrace";
  made.workers.push_back(mixed::WorkerCommands{slot, std::move(commands)});
  return made;
}

mixed::Schedule schedule_of(std::vector<mixed::Step> steps) {
  mixed::Schedule schedule;
  schedule.workers = 2;
  schedule.start = {{"raytrace", kFootprint}, {"raytrace", kFootprint}};
  schedule.phase = "start";
  schedule.workload = "raytrace";
  schedule.steps = std::move(steps);
  return schedule;
}

std::vector<std::uint32_t> permitted_cpus() {
  platform::RealSyscalls syscalls;
  platform::Affinity affinity(syscalls);
  auto permitted = affinity.permitted();
  return permitted.has_value() ? platform::ids_of(permitted.value()) : std::vector<std::uint32_t>{};
}

std::optional<RunReport> run(const mixed::Schedule& schedule, std::int64_t duration_ms,
                             const char* executable = LOADFORGE_BINARY,
                             Reporter* watcher = nullptr) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  auto machine = topology::take_snapshot(filesystem);
  if (!machine.has_value()) {
    ADD_FAILURE() << "no topology";
    return std::nullopt;
  }
  const std::vector<std::uint32_t> cpus = permitted_cpus();
  const workloads::BuiltinRegistry registry;
  RunOptions options;
  options.executable = executable;
  options.interval_ns = 20 * kMs;
  options.stale_after_ns = 10'000 * kMs;
  options.grace = Grace{6000 * kMs, 1000 * kMs};
  options.topology = &machine.value();
  options.verification.registry = &registry;
  options.verification.footprint_bytes = kFootprint;
  options.verification.line_bytes = 64;
  options.verification.cpus = cpus;
  options.schedule = &schedule;

  config::Config config;
  config.run.mode = config::Mode::kStress;
  config.run.duration = core::Duration{duration_ms};
  for (const mixed::Start& start : schedule.start) {
    config.workloads.push_back(
        config::Workload{start.workload, config::Threads::count(1), true, {}});
  }
  Quiet quiet;
  Controller controller(syscalls, watcher != nullptr ? *watcher : quiet);
  auto ran = controller.run(config, options);
  if (!ran.has_value()) {
    ADD_FAILURE() << describe(ran.error());
    return std::nullopt;
  }
  return ran.value();
}

bool has_record(const WorkerReport& worker, std::string_view text) {
  return std::ranges::any_of(worker.records, [&](const std::string& record) {
    return record.find(text) != std::string::npos;
  });
}

/// `asked` commands were sent, and the Stop at the deadline as well when the
/// worker was still there to be sent it: a worker that left on its own
/// duration before the controller's tick is reaped and sent nothing more,
/// which a loaded machine makes common (request_stop in orchestration.cpp).
void expect_sent(const WorkerReport& worker, std::uint64_t asked) {
  EXPECT_GE(worker.commands.sent, asked) << "slot " << worker.slot;
  EXPECT_LE(worker.commands.sent, asked + 1) << "slot " << worker.slot;
}

/// `asked` commands were done; the Stop at the deadline is one more when the
/// worker read it before it left on its own duration, and is not otherwise.
void expect_done(const WorkerReport& worker, std::uint64_t asked) {
  EXPECT_GE(worker.commands.done, asked) << "slot " << worker.slot;
  EXPECT_LE(worker.commands.done, asked + 1) << "slot " << worker.slot;
}

void expect_whole(const RunReport& report) {
  ASSERT_EQ(report.workers.size(), 2U);
  for (const WorkerReport& worker : report.workers) {
    EXPECT_EQ(worker.fate, WorkerFate::kCompleted) << describe(worker.fate);
    EXPECT_EQ(worker.wrong_results, 0U) << "slot " << worker.slot;
    EXPECT_TRUE(worker.tally_seen);
    EXPECT_GT(worker.tally.units_computed, 0U);
    EXPECT_TRUE(has_record(worker, "finished: "));
  }
}

TEST(ControlOverRealWorkers, StopEndsEveryWorkerCleanlyAtTheDeadlineWithNothingUnanswered) {
  const auto report = run(schedule_of({}), 1200);
  ASSERT_TRUE(report.has_value());
  expect_whole(*report);
  for (const WorkerReport& worker : report->workers) {
    EXPECT_EQ(worker.commands.unacknowledged, 0U) << "a Stop is the run's end, not a question";
    EXPECT_EQ(worker.commands.refused, 0U);
  }
}

TEST(ControlOverRealWorkers, PauseHoldsOneWorkerStillUntilResumeAndTheOtherDoesNotNotice) {
  const auto report = run(schedule_of({step(200, "paused", 0, {worker::Pause{}}),
                                       step(1500, "resumed", 0, {worker::Resume{}})}),
                          1900);
  ASSERT_TRUE(report.has_value());
  expect_whole(*report);
  const WorkerReport& told = report->workers[0];
  const WorkerReport& left = report->workers[1];
  expect_sent(told, 2);
  expect_done(told, 2);
  expect_sent(left, 0);
  EXPECT_LT(told.tally.units_computed, left.tally.units_computed)
      << "it worked for a fifth of the run";
}

TEST(ControlOverRealWorkers, ADutyCycleMakesOneWorkerDoLessThanItsNeighbour) {
  worker::SetDuty duty;
  duty.busy_permille = 150;
  duty.period_ns = 100 * kMs;
  const auto report = run(schedule_of({step(100, "slowed", 0, {duty})}), 1800);
  ASSERT_TRUE(report.has_value());
  expect_whole(*report);
  expect_done(report->workers[0], 1);
  EXPECT_LT(report->workers[0].tally.units_computed, report->workers[1].tally.units_computed);
}

TEST(ControlOverRealWorkers, ARebindMovesAWorkerAndItKeepsVerifyingWhatItWasDoing) {
  const std::vector<std::uint32_t> cpus = permitted_cpus();
  ASSERT_FALSE(cpus.empty());
  const auto report =
      run(schedule_of({step(300, "moved", 0, {worker::Rebind{{cpus.back()}}}),
                       step(700, "moved back", 0, {worker::Rebind{{cpus.front()}}})}),
          1400);
  ASSERT_TRUE(report.has_value());
  expect_whole(*report);
  expect_done(report->workers[0], 2);
  EXPECT_EQ(report->workers[0].commands.refused, 0U);
  expect_sent(report->workers[1], 0);
}

TEST(ControlOverRealWorkers, ASwitchChangesWhatAWorkerRunsAndTheTallyRunsOnAcrossIt) {
  const auto report = run(
      schedule_of(
          {step(400, "switched", 0, {worker::Switch{"compression", 77, kFootprint, 0, {1, 1}}})}),
      1500);
  ASSERT_TRUE(report.has_value());
  expect_whole(*report);
  expect_done(report->workers[0], 1);
  EXPECT_EQ(report->workers[0].not_performed, 0U) << "every unit on both sides was checked";
}

TEST(ControlOverRealWorkers, ARefusedCommandIsCountedAndNamedAndTheWorkerGoesOnWorking) {
  const std::vector<std::uint32_t> cpus = permitted_cpus();
  ASSERT_FALSE(cpus.empty());
  const std::uint32_t beyond = 1000;  // a CPU no machine here has
  ASSERT_TRUE(std::ranges::none_of(cpus, [&](std::uint32_t cpu) { return cpu == beyond; }));
  const auto report = run(schedule_of({step(300, "to nowhere", 0, {worker::Rebind{{beyond}}}),
                                       step(600, "to nothing", 0,
                                            {worker::Switch{"teapot", 1, kFootprint, 0, {1, 1}}})}),
                          1400);
  ASSERT_TRUE(report.has_value());
  expect_whole(*report);
  const WorkerReport& told = report->workers[0];
  expect_sent(told, 2);
  EXPECT_EQ(told.commands.refused, 2U);
  expect_done(told, 0);
  EXPECT_TRUE(has_record(told, "refused rebind")) << "the reason is in the worker's story";
  EXPECT_TRUE(has_record(told, "refused switch"));
  EXPECT_GT(told.tally.units_computed, 0U);
}

TEST(ControlOverRealWorkers, ACommandForEveryWorkerAtOnceIsAnsweredByEach) {
  mixed::Step both = step(300, "both", 0, {worker::Pause{}});
  both.workers.push_back(mixed::WorkerCommands{1, {worker::Pause{}}});
  mixed::Step again = step(700, "again", 0, {worker::Resume{}});
  again.workers.push_back(mixed::WorkerCommands{1, {worker::Resume{}}});
  const auto report = run(schedule_of({both, again}), 1200);
  ASSERT_TRUE(report.has_value());
  expect_whole(*report);
  for (const WorkerReport& worker : report->workers) {
    expect_done(worker, 2);
  }
}

/// Kills the second worker `after` into the run.
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
    if (line.substr(0, 7) != "spawned") {
      return;
    }
    std::istringstream rest{std::string{line.substr(line.find("pids") + 4)}};
    std::vector<pid_t> pids;
    pid_t pid = 0;
    while (rest >> pid) {
      pids.push_back(pid);
    }
    killer_ = std::thread([victim = pids.at(1), after = after_] {
      std::this_thread::sleep_for(after);
      ::kill(victim, SIGKILL);
    });
  }
  void run_finished(const RunReport& /*report*/) override {}
  void isolation_finished(const RunReport& /*report*/) override {}

 private:
  std::chrono::milliseconds after_;
  std::thread killer_;
};

TEST(ControlOverRealWorkers, AWorkerKilledBetweenTwoRotationsIsReportedAndItsNeighbourFinishes) {
  worker::SetDuty duty;
  duty.busy_permille = 900;
  duty.period_ns = 100 * kMs;
  mixed::Step both = step(300, "first", 0, {duty});
  both.workers.push_back(mixed::WorkerCommands{1, {duty}});
  mixed::Step later = step(1100, "second", 0, {worker::Pause{}});
  later.workers.push_back(mixed::WorkerCommands{1, {worker::Pause{}}});
  KillingReporter reporter(std::chrono::milliseconds{700});
  const auto report = run(schedule_of({both, later}), 1500, LOADFORGE_BINARY, &reporter);
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->workers.size(), 2U);
  EXPECT_EQ(report->workers[0].fate, WorkerFate::kCompleted);
  EXPECT_GE(report->workers[0].commands.done, 2U) << "and the Stop, when it came while paused";
  EXPECT_EQ(report->workers[1].fate, WorkerFate::kKilled);
  EXPECT_GE(report->workers[1].commands.done, 1U) << "it answered the first before it died";
  EXPECT_FALSE(all_clean(*report));
}

TEST(ControlOverRealWorkers, ACorruptionIsFoundAboutWhicheverWorkloadAWorkerRanWhenItHappened) {
  // The faulty worker corrupts every fifth unit of whatever it runs.
  ASSERT_EQ(::setenv("LOADFORGE_FAULT", "all", 1), 0);
  const auto report =
      run(schedule_of({step(500, "to compression", 0,
                            {worker::Switch{"compression", 5, kFootprint, 0, {1, 1}}})}),
          4200, LOADFORGE_FAULTY_WORKER);
  ::unsetenv("LOADFORGE_FAULT");
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->workers.size(), 2U);
  expect_done(report->workers[0], 1);
  const auto workload_of = [](const FindingReport& finding) {
    return std::visit([](const auto& each) { return each.provenance.workload.value_or("none"); },
                      finding.record);
  };
  std::size_t first = 0;
  std::size_t compression = 0;
  for (const FindingReport& finding : report->workers[0].findings) {
    const std::string name = workload_of(finding);
    EXPECT_TRUE(name == "raytrace" || name == "compression") << name;
    (name == "raytrace" ? first : compression)++;
  }
  EXPECT_GT(first, 0U) << "found before the switch, about raytrace";
  EXPECT_GT(compression, 0U) << "and after it, about compression";
  for (const FindingReport& finding : report->workers[1].findings) {
    EXPECT_EQ(workload_of(finding), "raytrace") << "the neighbour was never switched";
  }
  EXPECT_GT(report->workers[1].wrong_results, 0U);
}

}  // namespace
}  // namespace loadforge::controller
