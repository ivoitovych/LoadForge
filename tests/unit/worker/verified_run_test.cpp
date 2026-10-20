// SPDX-License-Identifier: GPL-3.0-or-later
//
// The verifying worker: how a unit of work is prepared (each refusal its own
// stage), how a tick runs (the order, the slice sizing, the frames), and every
// way the loop ends. The runner is the worker's own model of its contract
// (model_runner.hpp) or a scripted double; the registries are the real ones,
// because a registry's job is to name real things; the seam is the fake.

#include "worker/verified_run.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "platform/fpenv/environment.hpp"
#include "primitives/simd/dispatch.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/controller/consumer_models.hpp"
#include "unit/worker/model_runner.hpp"
#include "verification/oracle/registry/registry.hpp"
#include "verification/record.hpp"
#include "worker/frames.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/services.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::worker {
namespace {

using loadforge::ipc::Heartbeat;
using loadforge::ipc::HeartbeatRegion;
using loadforge::platform::PipeEnds;
using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kMs = 1'000'000;

// --- pure functions ---------------------------------------------------------------

TEST(NextSliceUnits, AimsAtTheTargetTimeButNeverMoreThanDoubles) {
  EXPECT_EQ(next_slice_units(10, 10 * kMs, 10 * kMs), 10U) << "on target";
  EXPECT_EQ(next_slice_units(10, 5 * kMs, 10 * kMs), 20U) << "twice too fast: double";
  EXPECT_EQ(next_slice_units(10, 1 * kMs, 10 * kMs), 20U) << "ten times too fast: still double";
  EXPECT_EQ(next_slice_units(10, 20 * kMs, 10 * kMs), 5U) << "twice too slow: half";
  EXPECT_EQ(next_slice_units(10, 1000 * kMs, 10 * kMs), 1U) << "never fewer than one";
  EXPECT_EQ(next_slice_units(1, 1000 * kMs, 10 * kMs), 1U);
}

TEST(NextSliceUnits, ASliceThatMeasuredNoTimeDoublesAndNothingExceedsTheCap) {
  EXPECT_EQ(next_slice_units(4, 0, 10 * kMs), 8U);
  EXPECT_EQ(next_slice_units(4, -5, 10 * kMs), 8U) << "a clock step backwards measures nothing";
  EXPECT_EQ(next_slice_units(kMaxSliceUnits, 1, 10 * kMs), kMaxSliceUnits);
  EXPECT_EQ(next_slice_units(kMaxSliceUnits, 0, 10 * kMs), kMaxSliceUnits);
  EXPECT_EQ(next_slice_units(kMaxSliceUnits / 2 + 1, 1, 10 * kMs), kMaxSliceUnits);
  EXPECT_EQ(next_slice_units(8, 1 * kMs, 0), 1U) << "a target of nothing is a target of a nanosecond";
}

TEST(PlacementsFor, OneThreadPerCpuCyclingAndNoneWhenNoCpusAreNamed) {
  auto none = placements_for(3, {});
  ASSERT_TRUE(none.has_value());
  ASSERT_EQ(none.value().size(), 3U);
  for (const ThreadPlacement& placement : none.value()) {
    EXPECT_FALSE(placement.has_value());
  }
  auto cycling = placements_for(3, {4, 9});
  ASSERT_TRUE(cycling.has_value());
  ASSERT_EQ(cycling.value().size(), 3U);
  EXPECT_TRUE(cycling.value()[0]->cpus.test(4));
  EXPECT_EQ(cycling.value()[0]->cpus.count(), 1U);
  EXPECT_TRUE(cycling.value()[1]->cpus.test(9));
  EXPECT_TRUE(cycling.value()[2]->cpus.test(4));
  auto beyond = placements_for(2, {4096});
  ASSERT_FALSE(beyond.has_value());
  EXPECT_FALSE(beyond.error().empty());
  auto no_threads = placements_for(0, {1});
  ASSERT_TRUE(no_threads.has_value());
  EXPECT_TRUE(no_threads.value().empty());
}

// --- the doubles --------------------------------------------------------------------

/// Floating-point registers that can be made to misbehave: writes that do not
/// take, a state someone else changes.
class ScriptedFp final : public fpenv::FpControl {
 public:
  [[nodiscard]] fpenv::FpState read() const override {
    ++reads;
    return state;
  }
  void write(const fpenv::FpState& wanted) override {
    if (!ignore_writes) {
      state.rounding = wanted.rounding;
      state.flush_to_zero = wanted.flush_to_zero;
      if (wanted.denormals_are_zero.has_value()) {
        state.denormals_are_zero = wanted.denormals_are_zero;
      }
      if (wanted.default_nan.has_value()) {
        state.default_nan = wanted.default_nan;
      }
    }
  }
  mutable int reads = 0;
  bool ignore_writes = false;
  fpenv::FpState state = fpenv::required_state(true, false);
};

/// The real oracles, except that creating one can change the registers
/// (a library loaded by a workload) or be refused.
class WrappedOracles final : public verification::OracleRegistry {
 public:
  explicit WrappedOracles(ScriptedFp& fp) : fp_(&fp) {}
  [[nodiscard]] std::vector<std::string> names() const override { return inner_.names(); }
  [[nodiscard]] core::Result<std::unique_ptr<verification::Oracle>, verification::OracleError>
  create(std::string_view name, const workloads::WorkloadConfig& config,
         const workloads::Plan& plan) const override {
    if (refuse) {
      return verification::OracleError{verification::OracleFault::kConfiguration, "scripted"};
    }
    if (dirty_the_registers) {
      fp_->state.flush_to_zero = true;
    }
    return inner_.create(name, config, plan);
  }
  bool refuse = false;
  bool dirty_the_registers = false;

 private:
  verification::BuiltinOracleRegistry inner_;
  ScriptedFp* fp_;
};

/// Threads that can refuse to start.
class RefusingThreads final : public Threads {
 public:
  [[nodiscard]] int start(pthread_t& handle, Entry entry, void* argument) override {
    return refuse ? EAGAIN : inner.start(handle, entry, argument);
  }
  void join(pthread_t handle) override { inner.join(handle); }
  bool refuse = false;
  PosixThreads inner;
};

/// What a scripted runner does, shared with the factory function that makes it
/// (a RunnerFactory is a plain function, so its state is a global).
struct Script {
  std::vector<verification::SliceReport> slices;  ///< One per run_slice, then repeated.
  bool slice_fails = false;
  std::optional<verification::SliceReport> final_report;
  bool finish_fails = false;
  bool factory_fails = false;
  int slices_run = 0;
  int finishes = 0;
  std::vector<std::uint64_t> asked;
  std::optional<verification::RunnerSetup> setup;
};
Script* g_script = nullptr;

class ScriptedRunner final : public verification::SliceRunner {
 public:
  core::Result<verification::SliceReport, verification::RunnerError> run_slice(
      const verification::SliceRequest& request) override {
    g_script->asked.push_back(request.units);
    ++g_script->slices_run;
    if (g_script->slice_fails) {
      return verification::RunnerError{verification::RunnerFault::kFinished, "scripted halt"};
    }
    verification::SliceReport report;
    if (!g_script->slices.empty()) {
      const auto at = static_cast<std::size_t>(g_script->slices_run - 1);
      report = g_script->slices[std::min(at, g_script->slices.size() - 1)];
    }
    total_ = total_ + report.tally;
    return report;
  }
  core::Result<verification::SliceReport, verification::RunnerError> finish() override {
    ++g_script->finishes;
    if (g_script->finish_fails) {
      return verification::RunnerError{verification::RunnerFault::kFinished, "scripted finish"};
    }
    return g_script->final_report.value_or(verification::SliceReport{});
  }
  [[nodiscard]] const verification::Tally& total() const noexcept override { return total_; }

 private:
  verification::Tally total_;
};

core::Result<std::unique_ptr<verification::SliceRunner>, verification::RunnerError>
make_scripted(const verification::RunnerSetup& setup) {
  g_script->setup = setup;
  if (g_script->factory_fails) {
    return verification::RunnerError{verification::RunnerFault::kResources, "scripted refusal"};
  }
  std::unique_ptr<verification::SliceRunner> runner = std::make_unique<ScriptedRunner>();
  return runner;
}

verification::VerificationNotPerformed unrun(const std::string& what) {
  verification::VerificationNotPerformed record;
  record.what = what;
  record.why = "scripted";
  return record;
}

verification::SliceReport slice_of(std::uint64_t units,
                                   std::vector<verification::ErrorRecord> records = {}) {
  verification::SliceReport report;
  report.tally.units_computed = units;
  report.tally.oracle_checked = units;
  report.tally.oracle_blocks = units;
  report.tally.integrity_checked = units;
  report.records = std::move(records);
  return report;
}

/// Everything a worker needs, over the fake: a region with the controller's
/// beat, a record pipe and a control pipe the test holds the other ends of.
struct Rig {
  FakeSyscalls syscalls;
  std::optional<HeartbeatRegion> region;
  PipeEnds records;
  PipeEnds control;
  ScriptedFp fp;
  WrappedOracles oracles{fp};
  workloads::BuiltinRegistry workloads;
  RefusingThreads threads;
  controller_testing::ModelProbe probe{simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kAvx2}};
  Script script;
  WorkerArgs worker;

  Rig() {
    g_script = &script;
    auto created = HeartbeatRegion::create(syscalls, 2);
    EXPECT_TRUE(created.has_value());
    region.emplace(std::move(created).value());
    EXPECT_TRUE(region->publish(ipc::kControllerSlot, Heartbeat{0, 1, 1, 1}).has_value());
    records = syscalls.create_pipe().value();
    control = syscalls.create_pipe().value();
    syscalls.advance_clocks_per_read(10 * kMs);
    worker.slot = 0;
    worker.workers = 2;
    worker.heartbeat_fd = region->fd();
    worker.record_fd = records.write_fd;
    worker.control_fd = control.read_fd;
    worker.supervisor_pid = FakeSyscalls::kFakeSupervisorPid;
    worker.pacing = Pacing{100 * kMs, 20 * kMs, 100'000 * kMs};
    WorkloadArgs unit;
    unit.workload = "compression";
    unit.seed = 3;
    unit.footprint_bytes = std::uint64_t{1} << 20U;
    unit.threads = 1;
    worker.workload = unit;
  }
  ~Rig() { g_script = nullptr; }

  [[nodiscard]] Services services(RunnerFactory factory = &make_scripted) {
    Services services;
    services.workloads = &workloads;
    services.oracles = &oracles;
    services.fp = &fp;
    services.threads = &threads;
    services.isa = &probe;
    services.make_runner = factory;
    return services;
  }

  [[nodiscard]] Session session() { return Session{&*region, worker.slot, control.read_fd, worker.pacing}; }

  core::Result<VerifiedOutcome, WorkerError> run(RunnerFactory factory = &make_scripted) {
    ipc::RecordWriter writer(syscalls, records.write_fd);
    const Services wired = services(factory);
    VerifiedRun verified(syscalls, writer, session(), worker, wired);
    return verified.run();
  }

  /// The frames the worker wrote: (type, payload).
  [[nodiscard]] std::vector<ipc::Record> received() const {
    std::vector<ipc::Record> out;
    ipc::RecordReader reader;
    std::vector<std::byte> raw;
    for (const char c : syscalls.pipe_contents(records.write_fd)) {
      raw.push_back(static_cast<std::byte>(c));
    }
    reader.feed(raw);
    while (true) {
      auto next = reader.next();
      EXPECT_TRUE(next.has_value());
      if (!next.has_value() || !next.value().has_value()) {
        break;
      }
      out.push_back(*next.value());
    }
    return out;
  }

  [[nodiscard]] std::vector<verification::Tally> tallies() const {
    std::vector<verification::Tally> out;
    for (const ipc::Record& record : received()) {
      if (record.type == kTallyFrameType) {
        out.push_back(decode_tally(record.payload).value());
      }
    }
    return out;
  }

  [[nodiscard]] std::vector<verification::ErrorRecord> findings() const {
    std::vector<verification::ErrorRecord> out;
    for (const ipc::Record& record : received()) {
      if (record.type == verification::kErrorRecordFrameType) {
        out.push_back(verification::decode(record.payload).value());
      }
    }
    return out;
  }
};

// --- prepare ---------------------------------------------------------------------

TEST(Prepare, BuildsEverythingInTheOrderItIsDestroyedInReverse) {
  Rig rig;
  const Services services = rig.services(&worker_testing::ModelRunner::make);
  auto prepared = prepare(rig.syscalls, *rig.worker.workload, {}, 7, services);
  ASSERT_TRUE(prepared.has_value()) << describe(prepared.error());
  EXPECT_NE(prepared.value().workload, nullptr);
  EXPECT_NE(prepared.value().oracle, nullptr);
  EXPECT_NE(prepared.value().executor, nullptr);
  EXPECT_NE(prepared.value().runner, nullptr);
  EXPECT_EQ(prepared.value().executor->threads(), 1U);
}

TEST(Prepare, TheRunnerIsToldWhereTheUnitRunsAndWhichUnitComesFirst) {
  Rig rig;
  rig.worker.workload->physical_core = 2;
  rig.worker.workload->numa_node = 1;
  rig.worker.workload->duty = verification::DutyCycle{3, 5};
  rig.worker.workload->line_bytes = 128;
  auto prepared = prepare(rig.syscalls, *rig.worker.workload, {0}, 99, rig.services());
  ASSERT_TRUE(prepared.has_value()) << describe(prepared.error());
  ASSERT_TRUE(rig.script.setup.has_value());
  const verification::RunnerSetup& setup = *rig.script.setup;
  EXPECT_EQ(setup.first_index, 99U);
  EXPECT_EQ(setup.line_bytes, 128U);
  EXPECT_EQ(setup.duty, (verification::DutyCycle{3, 5}));
  EXPECT_EQ(setup.base.workload, "compression");
  EXPECT_EQ(setup.base.workload_version, 1U);
  EXPECT_EQ(setup.base.seed, 3U);
  EXPECT_EQ(setup.base.isa, simd::IsaPath::kScalar);
  EXPECT_EQ(setup.base.logical_cpu, 0U) << "one CPU named: the unit ran there";
  EXPECT_EQ(setup.base.physical_core, 2U);
  EXPECT_EQ(setup.base.numa_node, 1U);
  EXPECT_EQ(setup.syscalls, &rig.syscalls);
}

TEST(Prepare, NoSingleCpuMeansNoClaimAboutWhichCpuAUnitRanOn) {
  Rig rig;
  auto unplaced = prepare(rig.syscalls, *rig.worker.workload, {}, 0, rig.services());
  ASSERT_TRUE(unplaced.has_value());
  EXPECT_FALSE(rig.script.setup->base.logical_cpu.has_value());
  rig.worker.workload->threads = 2;
  auto several = prepare(rig.syscalls, *rig.worker.workload, {0, 1}, 0, rig.services());
  ASSERT_TRUE(several.has_value()) << describe(several.error());
  EXPECT_FALSE(rig.script.setup->base.logical_cpu.has_value())
      << "two CPUs: the record names none, because it ran on both";
}

TEST(Prepare, EveryRefusalNamesItsStage) {
  const auto stage = [](Rig& rig) {
    auto prepared = prepare(rig.syscalls, *rig.worker.workload, {}, 0, rig.services());
    EXPECT_FALSE(prepared.has_value());
    return prepared.has_value() ? std::string{} : prepared.error().stage + ": " + prepared.error().detail;
  };
  {
    Rig rig;
    rig.fp.ignore_writes = true;
    rig.fp.state.flush_to_zero = true;
    const std::string refusal = stage(rig);
    EXPECT_EQ(refusal.substr(0, 28), "floating-point environment: ") << refusal;
    EXPECT_NE(refusal.find("flush_to_zero"), std::string::npos) << refusal;
  }
  {
    Rig rig;
    rig.worker.workload->isa = simd::IsaPath::kAvx512;  // The probe has scalar and AVX2.
    EXPECT_EQ(stage(rig).substr(0, 10), "isa path: ");
  }
  {
    Rig rig;
    rig.worker.workload->workload = "no_such";
    EXPECT_EQ(stage(rig), "workload: unknown workload: no workload named 'no_such'");
  }
  {
    Rig rig;
    rig.worker.workload->footprint_bytes = 1;
    EXPECT_EQ(stage(rig).substr(0, 10), "workload: ");
  }
  {
    Rig rig;
    rig.oracles.refuse = true;
    EXPECT_EQ(stage(rig),
              "oracle: configuration outside the specification: scripted");
  }
  {
    Rig rig;
    EXPECT_EQ(
        [&] {
          auto prepared = prepare(rig.syscalls, *rig.worker.workload, {4096}, 0, rig.services());
          return prepared.has_value() ? std::string{} : prepared.error().stage;
        }(),
        "executor");
  }
  {
    Rig rig;
    rig.threads.refuse = true;
    const std::string refusal = stage(rig);
    EXPECT_EQ(refusal.substr(0, 10), "executor: ") << refusal;
  }
  {
    Rig rig;
    rig.oracles.dirty_the_registers = true;  // Between the first check and the last.
    const std::string refusal = stage(rig);
    EXPECT_EQ(refusal.substr(0, 28), "floating-point environment: ") << refusal;
    EXPECT_NE(refusal.find("changed during start-up"), std::string::npos) << refusal;
  }
  {
    Rig rig;
    rig.script.factory_fails = true;
    EXPECT_EQ(stage(rig), "runner: runner resources: scripted refusal");
  }
}

TEST(Prepare, TheRegistersAreEstablishedBeforeAnyThreadAndAssertedAgainAfterAll) {
  Rig rig;
  rig.fp.state.flush_to_zero = true;  // Dirty on arrival: establishing repairs it.
  auto prepared = prepare(rig.syscalls, *rig.worker.workload, {}, 0, rig.services());
  ASSERT_TRUE(prepared.has_value()) << describe(prepared.error());
  EXPECT_FALSE(rig.fp.state.flush_to_zero);
  EXPECT_GE(rig.fp.reads, 3) << "establish, the pool's creator state, the assertion after";
}

// --- the loop ----------------------------------------------------------------------

TEST(VerifiedRunLoop, RunsSlicesUntilTheDurationAndSendsTheTallyAfterEach) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kDurationElapsed);
  EXPECT_GE(rig.script.slices_run, 3);
  EXPECT_EQ(rig.script.finishes, 1);
  EXPECT_EQ(outcome.value().tally.units_computed, static_cast<std::uint64_t>(rig.script.slices_run));
  EXPECT_EQ(outcome.value().outcome.progress, outcome.value().tally.units_computed);
  EXPECT_TRUE(outcome.value().note.empty());
  const std::vector<verification::Tally> tallies = rig.tallies();
  ASSERT_EQ(tallies.size(), static_cast<std::size_t>(rig.script.slices_run) + 1)
      << "one after every slice, and the last after finish()";
  EXPECT_EQ(tallies.back(), outcome.value().tally);
  for (std::size_t at = 1; at < tallies.size(); ++at) {
    EXPECT_GE(tallies[at].units_computed, tallies[at - 1].units_computed) << "cumulative";
  }
  auto beat = rig.region->observe(ipc::worker_slot(0));
  ASSERT_TRUE(beat.has_value() && beat.value().has_value());
  EXPECT_EQ(beat.value()->progress, outcome.value().tally.units_computed - 0U) << "the last beat";
}

TEST(VerifiedRunLoop, TheFirstSliceIsOneUnitAndLaterOnesAreSizedFromTheTimeTheyTook) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  ASSERT_TRUE(rig.run().has_value());
  ASSERT_GE(rig.script.asked.size(), 3U);
  EXPECT_EQ(rig.script.asked[0], 1U);
  // A slice takes the 20 ms two clock reads cost here against a target of
  // half the 20 ms interval: shorter or equal to what came before.
  EXPECT_LE(rig.script.asked[1], 2U);
  for (const std::uint64_t units : rig.script.asked) {
    EXPECT_GE(units, 1U);
    EXPECT_LE(units, kMaxSliceUnits);
  }
}

TEST(VerifiedRunLoop, EveryRecordASliceMadeIsSentBeforeItsTally) {
  Rig rig;
  rig.script.slices = {slice_of(1, {unrun("a"), unrun("b")})};
  ASSERT_TRUE(rig.run().has_value());
  const std::vector<ipc::Record> frames = rig.received();
  ASSERT_GE(frames.size(), 3U);
  EXPECT_EQ(frames[0].type, verification::kErrorRecordFrameType);
  EXPECT_EQ(frames[1].type, verification::kErrorRecordFrameType);
  EXPECT_EQ(frames[2].type, kTallyFrameType);
  const std::vector<verification::ErrorRecord> findings = rig.findings();
  ASSERT_GE(findings.size(), 2U);
  EXPECT_EQ(std::get<verification::VerificationNotPerformed>(findings[0]).what, "a");
  EXPECT_EQ(std::get<verification::VerificationNotPerformed>(findings[1]).what, "b");
}

TEST(VerifiedRunLoop, WhatTheWholeRunDidNotCheckIsSentBeforeTheWorkerIsDone) {
  Rig rig;
  verification::SliceReport last;
  last.records.emplace_back(unrun("oracle over compression units"));
  rig.script.final_report = last;
  ASSERT_TRUE(rig.run().has_value());
  const std::vector<verification::ErrorRecord> findings = rig.findings();
  ASSERT_FALSE(findings.empty());
  EXPECT_EQ(std::get<verification::VerificationNotPerformed>(findings.back()).what,
            "oracle over compression units");
  const std::vector<ipc::Record> frames = rig.received();
  EXPECT_EQ(frames.back().type, kTallyFrameType) << "and the final tally after it";
}

TEST(VerifiedRunLoop, AStopRequestEndsTheRunAfterAFinalReport) {
  Rig rig;
  rig.syscalls.deliver_to_pipe(rig.control.read_fd, "x");
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kStopRequested);
  EXPECT_EQ(rig.script.slices_run, 0) << "the pipe is looked at before any work";
  EXPECT_EQ(rig.script.finishes, 1);
}

TEST(VerifiedRunLoop, AControllerThatHungUpEndsTheRunWithoutAFinalReport) {
  Rig rig;
  ASSERT_TRUE(rig.syscalls.close(rig.control.write_fd).has_value());
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
  EXPECT_EQ(rig.script.finishes, 0) << "nobody is listening to what it would say";
}

TEST(VerifiedRunLoop, AStaleControllerEndsTheRunTheSameWay) {
  Rig rig;
  rig.worker.pacing = Pacing{100'000 * kMs, 20 * kMs, 50 * kMs};  // The beat at 0 goes stale.
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerStale);
  EXPECT_EQ(rig.script.finishes, 0);
}

TEST(VerifiedRunLoop, ARunnerThatStoppedItselfIsAReasonToEndAndStillFinishes) {
  Rig rig;
  rig.script.slice_fails = true;
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kRunnerStopped);
  EXPECT_EQ(outcome.value().note, "scripted halt");
  EXPECT_EQ(rig.script.finishes, 1);
}

TEST(VerifiedRunLoop, FailuresNameTheirStage) {
  {
    Rig rig;
    rig.script.finish_fails = true;
    rig.syscalls.deliver_to_pipe(rig.control.read_fd, "x");
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "runner");
    EXPECT_EQ(outcome.error().detail, "finish: scripted finish");
  }
  {
    Rig rig;
    rig.syscalls.fail_poll(EBADF);
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "control pipe");
  }
  {
    Rig rig;
    rig.syscalls.fail_clock(CLOCK_MONOTONIC_RAW, EINVAL);
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "clock");
  }
  {
    Rig rig;
    rig.syscalls.fail_clock_on_read(CLOCK_MONOTONIC, 2, EIO);  // The first tick's reading.
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "clock");
    EXPECT_EQ(rig.script.slices_run, 0);
  }
  {
    Rig rig;
    rig.syscalls.fail_clock_on_read(CLOCK_MONOTONIC, 3, EIO);  // After the first slice.
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "clock");
    EXPECT_EQ(rig.script.slices_run, 1);
  }
  {
    Rig rig;
    for (const int clock_id : {CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC, CLOCK_BOOTTIME}) {
      rig.syscalls.set_clock(clock_id, platform::TimeSpec{100, 0});
    }
    rig.syscalls.advance_clocks_per_read(-10 * kMs);
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "clock");
    EXPECT_NE(outcome.error().detail.find("backwards"), std::string::npos);
  }
  {
    Rig rig;
    rig.worker.slot = 5;  // The region has two worker slots.
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "heartbeat");
  }
  {
    Rig rig;
    auto outcome = rig.run([](const verification::RunnerSetup&) {
      return core::Result<std::unique_ptr<verification::SliceRunner>, verification::RunnerError>{
          verification::RunnerError{verification::RunnerFault::kConfiguration, "scripted"}};
    });
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "runner");
  }
}

TEST(VerifiedRunLoop, ARecordPipeWithNoReaderIsTheControllerGoneAnythingElseIsTheWorkers) {
  {
    Rig rig;
    rig.script.slices = {slice_of(1, {unrun("a")})};
    ASSERT_TRUE(rig.syscalls.close(rig.records.read_fd).has_value());
    auto outcome = rig.run();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
    EXPECT_EQ(rig.script.finishes, 0);
  }
  {
    Rig rig;
    rig.script.slices = {slice_of(1, {unrun("a")})};
    rig.syscalls.script_writes({{0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
    EXPECT_NE(outcome.error().detail.find("Input/output error"), std::string::npos);
  }
  {
    // The tally frame is the one refused.
    Rig rig;
    rig.script.slices = {slice_of(1)};
    rig.syscalls.script_writes({{0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
  }
  {
    // The final report is the one refused.
    Rig rig;
    verification::SliceReport last;
    last.records.emplace_back(unrun("late"));
    rig.script.final_report = last;
    rig.syscalls.deliver_to_pipe(rig.control.read_fd, "x");
    rig.syscalls.script_writes({{0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
  }
  {
    Rig rig;
    verification::SliceReport last;
    last.records.emplace_back(unrun("late"));
    rig.script.final_report = last;
    rig.syscalls.deliver_to_pipe(rig.control.read_fd, "x");
    ASSERT_TRUE(rig.syscalls.close(rig.records.read_fd).has_value());
    auto outcome = rig.run();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
  }
}

// --- with the worker's model of the runner and the real workload ------------------

TEST(VerifiedRunLoop, ARealWorkloadThroughTheModelRunnerIsCheckedAndReportsNothingWrong) {
  Rig rig;
  rig.worker.workload->duty = verification::DutyCycle{1, 1};
  auto outcome = rig.run(&worker_testing::ModelRunner::make);
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kDurationElapsed);
  EXPECT_GT(outcome.value().tally.units_computed, 0U);
  EXPECT_EQ(outcome.value().tally.oracle_checked, outcome.value().tally.units_computed);
  EXPECT_EQ(outcome.value().tally.oracle_failed, 0U);
  EXPECT_TRUE(rig.findings().empty());
}

TEST(VerifiedRunLoop, ARunThatCheckedNothingSaysSoOnTheWire) {
  Rig rig;
  rig.worker.workload->duty = verification::DutyCycle{0, 0};
  rig.syscalls.deliver_to_pipe(rig.control.read_fd, "x");
  auto outcome = rig.run(&worker_testing::ModelRunner::make);
  ASSERT_TRUE(outcome.has_value());
  const std::vector<verification::ErrorRecord> findings = rig.findings();
  ASSERT_EQ(findings.size(), 2U);
  for (const verification::ErrorRecord& record : findings) {
    EXPECT_FALSE(verification::is_wrong_result(record));
  }
}

}  // namespace
}  // namespace loadforge::worker
