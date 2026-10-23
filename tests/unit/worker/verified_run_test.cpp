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
#include <functional>
#include <limits>
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
#include "unit/worker/control_frames.hpp"
#include "unit/worker/model_runner.hpp"
#include "verification/oracle/registry/registry.hpp"
#include "verification/record.hpp"
#include "worker/control.hpp"
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

TEST(NextSliceUnits, AnIntervalOfHoursDoesNotWrapTheProduct) {
  // 2^20 units times a target of 2^44 ns is 2^64: it wrapped to zero, and the
  // slice shrank to one unit instead of staying at the cap.
  constexpr std::int64_t kWraps = std::int64_t{1} << 44U;
  EXPECT_EQ(next_slice_units(kMaxSliceUnits, 1, kWraps), kMaxSliceUnits);
  EXPECT_EQ(next_slice_units(kMaxSliceUnits, kWraps, kWraps), kMaxSliceUnits)
      << "on target at the cap";
  EXPECT_EQ(next_slice_units(kMaxSliceUnits, 2 * kWraps, kWraps), kMaxSliceUnits / 2);
  // The largest target and measurement the type holds, and the largest last
  // slice: the answer is the cap, not a wrapped number.
  constexpr std::int64_t kLargest = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(next_slice_units(kMaxSliceUnits, 1, kLargest), kMaxSliceUnits);
  EXPECT_EQ(next_slice_units(kMaxSliceUnits, kLargest, kLargest), kMaxSliceUnits);
  EXPECT_EQ(next_slice_units(std::numeric_limits<std::uint64_t>::max(), 1, kLargest),
            kMaxSliceUnits)
      << "doubling a last slice past 2^63 does not wrap either";
}

TEST(NextSliceUnits, ASliceThatMeasuredNoTimeDoublesAndNothingExceedsTheCap) {
  EXPECT_EQ(next_slice_units(4, 0, 10 * kMs), 8U);
  EXPECT_EQ(next_slice_units(4, -5, 10 * kMs), 8U) << "a clock step backwards measures nothing";
  EXPECT_EQ(next_slice_units(kMaxSliceUnits, 1, 10 * kMs), kMaxSliceUnits);
  EXPECT_EQ(next_slice_units(kMaxSliceUnits, 0, 10 * kMs), kMaxSliceUnits);
  EXPECT_EQ(next_slice_units(kMaxSliceUnits / 2 + 1, 1, 10 * kMs), kMaxSliceUnits);
  EXPECT_EQ(next_slice_units(8, 1 * kMs, 0), 1U)
      << "a target of nothing is a target of a nanosecond";
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
    if (refuse_next > 0) {
      --refuse_next;
      return EAGAIN;
    }
    return refuse ? EAGAIN : inner.start(handle, entry, argument);
  }
  void join(pthread_t handle) override { inner.join(handle); }
  bool refuse = false;
  int refuse_next = 0;  ///< The next this many starts are refused, then they work again.
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
  verification::MemoryLock lock;  ///< What every scripted runner says it did about locking.
  /// Called as each scripted runner is made, with the setup it was made for, and as each of
  /// its slices runs, with the slice's number from one: the way a test acts in the middle
  /// of a run (sends the next message, then) without a clock of its own.
  std::function<bool(const verification::RunnerSetup&)> refuse_setup;
  std::function<void(int)> on_slice;
  std::vector<verification::RunnerSetup> setups;  ///< Every runner made, in order.
  std::vector<std::uint64_t> slice_starts;        ///< Each slice's first unit, from the setups.
};
/// The script the factory's runners follow: a RunnerFactory is a plain function,
/// so the state it reads cannot be a capture.
Script*& current_script() {
  static Script* script = nullptr;
  return script;
}

class ScriptedRunner final : public verification::SliceRunner {
 public:
  core::Result<verification::SliceReport, verification::RunnerError> run_slice(
      const verification::SliceRequest& request) override {
    current_script()->asked.push_back(request.units);
    ++current_script()->slices_run;
    if (current_script()->on_slice) {
      current_script()->on_slice(current_script()->slices_run);
    }
    if (current_script()->slice_fails) {
      return verification::RunnerError{verification::RunnerFault::kFinished, "scripted halt"};
    }
    verification::SliceReport report;
    if (!current_script()->slices.empty()) {
      const auto at = static_cast<std::size_t>(current_script()->slices_run - 1);
      report = current_script()->slices[std::min(at, current_script()->slices.size() - 1)];
    }
    total_ = total_ + report.tally;
    return report;
  }
  core::Result<verification::SliceReport, verification::RunnerError> finish() override {
    ++current_script()->finishes;
    if (current_script()->finish_fails) {
      return verification::RunnerError{verification::RunnerFault::kFinished, "scripted finish"};
    }
    return current_script()->final_report.value_or(verification::SliceReport{});
  }
  [[nodiscard]] const verification::Tally& total() const noexcept override { return total_; }
  [[nodiscard]] const verification::MemoryLock& memory_lock() const noexcept override {
    return current_script()->lock;
  }

 private:
  verification::Tally total_;
};

core::Result<std::unique_ptr<verification::SliceRunner>, verification::RunnerError> make_scripted(
    const verification::RunnerSetup& setup) {
  current_script()->setup = setup;
  current_script()->setups.push_back(setup);
  if (current_script()->factory_fails ||
      (current_script()->refuse_setup && current_script()->refuse_setup(setup))) {
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
  /// A machine that can run every path this build compiled, for the tests that must not
  /// name one.
  controller_testing::ModelProbe wide_probe{simd::compiled_paths()};
  bool use_wide_probe = false;
  Script script;
  WorkerArgs worker;

  Rig() {
    current_script() = &script;
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
  ~Rig() { current_script() = nullptr; }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;
  Rig(Rig&&) = delete;
  Rig& operator=(Rig&&) = delete;

  [[nodiscard]] Services services(RunnerFactory factory = &make_scripted) {
    Services services;
    services.workloads = &workloads;
    services.oracles = &oracles;
    services.fp = &fp;
    services.threads = &threads;
    services.isa = use_wide_probe ? &wide_probe : &probe;
    services.make_runner = factory;
    return services;
  }

  [[nodiscard]] Session session() {
    return Session{&*region, worker.slot, control.read_fd, worker.pacing, nullptr};
  }

  /// A message as the controller frames it, delivered to the worker.
  void send(const ControlMessage& message) {
    syscalls.deliver_to_pipe(control.read_fd, worker_testing::control_frame(message));
  }

  /// The acknowledgements the worker sent, in order.
  [[nodiscard]] std::vector<ControlAck> answers() const {
    std::vector<ControlAck> out;
    for (const ipc::Record& record : received()) {
      if (record.type == kControlAckFrameType) {
        out.push_back(decode_ack(record.payload).value());
      }
    }
    return out;
  }

  /// The types of the frames the worker sent, in order: the shape of what it said.
  [[nodiscard]] std::vector<std::uint16_t> frame_types() const {
    std::vector<std::uint16_t> out;
    for (const ipc::Record& record : received()) {
      out.push_back(record.type);
    }
    return out;
  }

  core::Result<VerifiedOutcome, WorkerError> run(RunnerFactory factory = &make_scripted) {
    ipc::RecordWriter writer(syscalls, records.write_fd);
    const Services wired = services(factory);
    VerifiedRun verified(syscalls, writer, session(), worker, *worker.workload, wired);
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
  rig.worker.workload->physical_package = 1;
  rig.worker.workload->physical_core = 2;
  rig.worker.workload->numa_node = 1;
  rig.worker.workload->decomposition = 0;  // Asked for the default: the record says what it was.
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
  EXPECT_EQ(setup.base.physical_package, 1U) << "a core is a package and a core";
  EXPECT_EQ(setup.base.physical_core, 2U);
  EXPECT_EQ(setup.base.numa_node, 1U);
  // M2 contract corrections, 3: the decomposition the workload RESOLVED (never
  // the 0 that asked for the default) and the footprint, for a re-run.
  EXPECT_EQ(setup.base.decomposition, prepared.value().workload->plan().decomposition);
  EXPECT_NE(setup.base.decomposition, 0U);
  EXPECT_EQ(setup.base.footprint_bytes, rig.worker.workload->footprint_bytes);
  EXPECT_EQ(setup.lock, rig.worker.workload->lock);
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
    return prepared.has_value() ? std::string{}
                                : prepared.error().stage + ": " + prepared.error().detail;
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
    EXPECT_EQ(stage(rig), "oracle: configuration outside the specification: scripted");
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
  EXPECT_EQ(outcome.value().tally.units_computed,
            static_cast<std::uint64_t>(rig.script.slices_run));
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
  ASSERT_GE(frames.size(), 4U);
  EXPECT_EQ(frames[0].type, kMemoryFrameType) << "told once, before the first slice";
  EXPECT_EQ(frames[1].type, verification::kErrorRecordFrameType);
  EXPECT_EQ(frames[2].type, verification::kErrorRecordFrameType);
  EXPECT_EQ(frames[3].type, kTallyFrameType);
  const std::vector<verification::ErrorRecord> findings = rig.findings();
  ASSERT_GE(findings.size(), 2U);
  EXPECT_EQ(std::get<verification::VerificationNotPerformed>(findings[0]).what, "a");
  EXPECT_EQ(std::get<verification::VerificationNotPerformed>(findings[1]).what, "b");
}

// M2 contract corrections, 2: what the runner did about locking is told to the
// controller, once, as it is; and what the worker was told to do about it is
// handed to the runner.
TEST(VerifiedRunLoop, TheLockPolicyGoesToTheRunnerAndWhatCameOfItToTheController) {
  Rig rig;
  rig.worker.workload->lock = verification::LockPolicy::kTry;
  rig.script.lock = verification::MemoryLock{verification::LockPolicy::kTry,
                                             verification::LockOutcome::kFellBack, "mlock refused"};
  ASSERT_TRUE(rig.run().has_value());
  ASSERT_TRUE(rig.script.setup.has_value());
  EXPECT_EQ(rig.script.setup->lock, verification::LockPolicy::kTry);
  const std::vector<ipc::Record> frames = rig.received();
  ASSERT_FALSE(frames.empty());
  ASSERT_EQ(frames[0].type, kMemoryFrameType);
  const auto told = decode_memory(frames[0].payload);
  ASSERT_TRUE(told.has_value()) << told.error();
  EXPECT_EQ(told.value(), rig.script.lock);
  EXPECT_EQ(std::count_if(frames.begin(), frames.end(),
                          [](const ipc::Record& f) { return f.type == kMemoryFrameType; }),
            1);
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
  rig.send({1, Stop{}});
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

TEST(VerifiedRunLoop, ALeaveBecauseTheControllerIsGoneReportsTheWorkDone) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  rig.worker.pacing = Pacing{100'000 * kMs, 20 * kMs, 200 * kMs};  // the beat at 0 goes stale
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  ASSERT_EQ(outcome.value().outcome.reason, StopReason::kControllerStale);
  EXPECT_GT(outcome.value().outcome.progress, 0U)
      << "units were computed before the controller went";
  EXPECT_EQ(outcome.value().tally.units_computed, outcome.value().outcome.progress);
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
    rig.send({1, Stop{}});
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

// A write script that lets one frame through whole (the memory frame, the first the worker
// sends) and then refuses the next with the errno given.
constexpr std::size_t kEverything = std::size_t{1} << 20U;

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
    rig.syscalls.script_writes({{kEverything, 0}, {0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
    EXPECT_NE(outcome.error().detail.find("Input/output error"), std::string::npos);
  }
  {
    // The memory frame, the first of all, is the one refused.
    Rig rig;
    rig.syscalls.script_writes({{0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
    EXPECT_EQ(rig.script.slices_run, 0);
  }
  {
    // The reader is gone after the memory frame: the controller left.
    Rig rig;
    rig.script.slices = {slice_of(1, {unrun("a")})};
    rig.syscalls.script_writes({{kEverything, 0}, {0, EPIPE}});
    auto outcome = rig.run();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
  }
  {
    // The tally frame is the one refused.
    Rig rig;
    rig.script.slices = {slice_of(1)};
    rig.syscalls.script_writes({{kEverything, 0}, {0, EIO}});
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
    rig.send({1, Stop{}});
    rig.syscalls.script_writes({{kEverything, 0}, {kEverything, 0}, {0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
  }
  {
    Rig rig;
    verification::SliceReport last;
    last.records.emplace_back(unrun("late"));
    rig.script.final_report = last;
    rig.send({1, Stop{}});
    rig.syscalls.script_writes({{kEverything, 0}, {kEverything, 0}, {0, EPIPE}});
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
  rig.send({1, Stop{}});
  auto outcome = rig.run(&worker_testing::ModelRunner::make);
  ASSERT_TRUE(outcome.has_value());
  const std::vector<verification::ErrorRecord> findings = rig.findings();
  ASSERT_EQ(findings.size(), 2U);
  for (const verification::ErrorRecord& record : findings) {
    EXPECT_FALSE(verification::is_wrong_result(record));
  }
}

// --- the control commands (worker/control.hpp) ---------------------------------------

/// The phase and progress of the worker's last beat.
Heartbeat last_beat(Rig& rig) {
  auto observed = rig.region->observe(ipc::worker_slot(0));
  EXPECT_TRUE(observed.has_value() && observed.value().has_value());
  return observed.has_value() && observed.value().has_value() ? *observed.value() : Heartbeat{};
}

TEST(VerifiedRunControl, AStopIsAnsweredBeforeTheRunsLastWord) {
  Rig rig;
  rig.send({7, Stop{}});
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kStopRequested);
  EXPECT_EQ(rig.frame_types(),
            (std::vector<std::uint16_t>{kMemoryFrameType, kControlAckFrameType, kTallyFrameType}))
      << "acknowledged before the final tally, which is before the worker's finished record";
  EXPECT_EQ(rig.answers(), (std::vector<ControlAck>{{7, AckStatus::kDone, ""}}));
}

TEST(VerifiedRunControl, APausedWorkerRunsNoSliceAndBeatsWithItsProgressWhereItWas) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 2) {
      rig.send({5, Pause{}});
    }
  };
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kDurationElapsed);
  EXPECT_EQ(rig.script.slices_run, 2) << "no slice after the pause";
  EXPECT_EQ(rig.answers(), (std::vector<ControlAck>{{5, AckStatus::kDone, ""}}));
  const Heartbeat beat = last_beat(rig);
  EXPECT_EQ(beat.phase, NullWorkload::kPhasePaused);
  EXPECT_EQ(beat.progress, 2U) << "unchanged since the last slice";
  // Every tick after the pause waited on the control pipe for the whole interval.
  const std::vector<int>& waits = rig.syscalls.poll_timeouts();
  ASSERT_GE(waits.size(), 4U);
  EXPECT_EQ(waits.back(), 20);
}

TEST(VerifiedRunControl, AResumeRunsSlicesAgainAndOneWhileRunningIsAcknowledgedDone) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  rig.send({1, Resume{}});  // While running.
  rig.send({2, Pause{}});
  rig.send({3, Resume{}});
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_GE(rig.script.slices_run, 2);
  EXPECT_EQ(rig.answers(),
            (std::vector<ControlAck>{
                {1, AckStatus::kDone, ""}, {2, AckStatus::kDone, ""}, {3, AckStatus::kDone, ""}}));
  EXPECT_EQ(last_beat(rig).phase, NullWorkload::kPhaseRunning);
}

TEST(VerifiedRunControl, ADutyCycleRunsSlicesInTheBusyPartAndWaitsOutTheIdlePart) {
  Rig continuous;
  continuous.worker.pacing = Pacing{600 * kMs, 20 * kMs, 100'000 * kMs};
  continuous.script.slices = {slice_of(1)};
  ASSERT_TRUE(continuous.run().has_value());

  Rig duty;
  duty.worker.pacing = Pacing{600 * kMs, 20 * kMs, 100'000 * kMs};
  duty.script.slices = {slice_of(1)};
  duty.send({1, SetDuty{250, 200 * kMs}});
  auto outcome = duty.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(duty.answers(), (std::vector<ControlAck>{{1, AckStatus::kDone, ""}}));
  EXPECT_GT(duty.script.slices_run, 0);
  EXPECT_LT(duty.script.slices_run, continuous.script.slices_run)
      << "a quarter of every period is busy";
  // The idle part is waited out on the control pipe, a tick at most one interval; a
  // worker with work to do only looks (0).
  int waited = 0;
  for (const int timeout : duty.syscalls.poll_timeouts()) {
    EXPECT_LE(timeout, 20);
    waited += timeout > 0 ? 1 : 0;
  }
  EXPECT_GT(waited, 0);
  for (const int timeout : continuous.syscalls.poll_timeouts()) {
    EXPECT_EQ(timeout, 0);
  }
  EXPECT_EQ(last_beat(duty).phase, NullWorkload::kPhaseRunning)
      << "idling by the duty cycle is not a pause";
}

TEST(VerifiedRunControl, AnIdlePartShorterThanAMillisecondIsWaitedAsOne) {
  Rig rig;
  rig.worker.pacing = Pacing{100 * kMs, 20 * kMs, 100'000 * kMs};
  rig.script.slices = {slice_of(1)};
  // Half of every 1.5 ms: the idle part is 0.75 ms, so the wait that follows a
  // busy tick is one millisecond, not none (which would spin).
  rig.send({1, SetDuty{500, 1'500'000}});
  ASSERT_TRUE(rig.run().has_value());
  const std::vector<int>& waits = rig.syscalls.poll_timeouts();
  EXPECT_NE(std::find(waits.begin(), waits.end(), 1), waits.end());
  EXPECT_EQ(std::count_if(waits.begin(), waits.end(), [](int w) { return w < 0 || w > 20; }), 0);
}

TEST(VerifiedRunControl, ARebindMakesTheRunnerAgainFromTheNextUnitOnTheNewCpus) {
  Rig rig;
  rig.worker.workload->physical_package = 1;
  rig.worker.workload->physical_core = 2;
  rig.worker.workload->numa_node = 1;
  rig.script.slices = {slice_of(1)};
  verification::SliceReport last;
  last.next_index = 42;
  rig.script.final_report = last;
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.send({4, Rebind{{0}}});
    }
  };
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  ASSERT_EQ(rig.script.setups.size(), 2U);
  EXPECT_EQ(rig.script.setups[0].base.physical_package, 1U);
  EXPECT_EQ(rig.script.setups[1].first_index, 42U) << "from the next unit, none run twice";
  EXPECT_EQ(rig.script.setups[1].base.logical_cpu, 0U) << "one CPU named";
  EXPECT_FALSE(rig.script.setups[1].base.physical_package.has_value())
      << "a package that was true before the move is not claimed after it";
  EXPECT_FALSE(rig.script.setups[1].base.physical_core.has_value());
  EXPECT_FALSE(rig.script.setups[1].base.numa_node.has_value());
  EXPECT_EQ(rig.script.setups[1].base.workload, "compression");
  EXPECT_EQ(rig.answers(), (std::vector<ControlAck>{{4, AckStatus::kDone, ""}}));
  // The lock is told again for the runner that replaced the first.
  const std::vector<std::uint16_t> types = rig.frame_types();
  EXPECT_EQ(std::count(types.begin(), types.end(), kMemoryFrameType), 2);
  // Progress and the tally run on across the replacement: cumulative.
  const std::vector<verification::Tally> tallies = rig.tallies();
  for (std::size_t at = 1; at < tallies.size(); ++at) {
    EXPECT_GE(tallies[at].units_computed, tallies[at - 1].units_computed);
  }
  EXPECT_EQ(tallies.back().units_computed, static_cast<std::uint64_t>(rig.script.slices_run));
  EXPECT_EQ(outcome.value().tally, tallies.back());
}

TEST(VerifiedRunControl, ARebindTheKernelRefusesIsRefusedAndTheWorkerStaysWhereItWas) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.threads.refuse_next = 1;  // The new executor's thread cannot be placed.
      rig.send({4, Rebind{{1}}});
    }
  };
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kDurationElapsed);
  const std::vector<ControlAck> answers = rig.answers();
  ASSERT_EQ(answers.size(), 1U);
  EXPECT_EQ(answers[0].status, AckStatus::kRefused);
  EXPECT_EQ(answers[0].detail.substr(0, 10), "executor: ") << answers[0].detail;
  ASSERT_EQ(rig.script.setups.size(), 2U) << "the old placement made again";
  EXPECT_FALSE(rig.script.setups[1].base.logical_cpu.has_value());
  EXPECT_GT(rig.script.slices_run, 1) << "and the run went on";
}

/// A worker running dense_linear on a path other than scalar, if this build has one.
std::optional<simd::IsaPath> vector_path_of(const workloads::BuiltinRegistry& registry) {
  const simd::IsaSet paths = registry.descriptor("dense_linear").value().paths;
  for (const simd::IsaPath path : simd::kPreference) {
    if (path != simd::IsaPath::kScalar && paths.contains(path) &&
        simd::compiled_paths().contains(path)) {
      return path;
    }
  }
  return std::nullopt;
}

TEST(VerifiedRunControl, ASwitchStartsTheNamedWorkloadWithItsSeedFootprintAndDutyCycle) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.send({6, Switch{"sort", 77, std::uint64_t{8} << 20U, 0, {3, 5}}});
    }
  };
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(rig.answers(), (std::vector<ControlAck>{{6, AckStatus::kDone, ""}}));
  ASSERT_EQ(rig.script.setups.size(), 2U);
  const verification::RunnerSetup& setup = rig.script.setups[1];
  EXPECT_EQ(setup.base.workload, "sort");
  EXPECT_EQ(setup.base.seed, 77U);
  EXPECT_EQ(setup.base.footprint_bytes, std::uint64_t{8} << 20U);
  EXPECT_EQ(setup.duty, (verification::DutyCycle{3, 5}));
  EXPECT_EQ(setup.lock, rig.worker.workload->lock) << "the lock policy is the run's";
  EXPECT_EQ(setup.line_bytes, rig.worker.workload->line_bytes);
}

TEST(VerifiedRunControl, ASwitchKeepsThePathTheWorkloadHasAndOtherwiseTakesTheBestBothHave) {
  Rig rig;
  const std::optional<simd::IsaPath> vector = vector_path_of(rig.workloads);
  if (!vector.has_value()) {
    GTEST_SKIP() << "this build has no vector path for dense_linear";
  }
  rig.use_wide_probe = true;
  rig.worker.workload->workload = "dense_linear";
  rig.worker.workload->footprint_bytes = std::uint64_t{8} << 20U;
  rig.worker.workload->isa = *vector;
  rig.script.slices = {slice_of(1)};
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.send({1, Switch{"sort", 1, std::uint64_t{8} << 20U, 0, {}}});  // Scalar only.
    }
    if (slice == 2) {
      rig.send({2, Switch{"dense_linear", 1, std::uint64_t{8} << 20U, 0, {}}});
    }
  };
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  EXPECT_EQ(rig.answers(),
            (std::vector<ControlAck>{{1, AckStatus::kDone, ""}, {2, AckStatus::kDone, ""}}));
  ASSERT_EQ(rig.script.setups.size(), 3U);
  EXPECT_EQ(rig.script.setups[0].base.isa, *vector);
  EXPECT_EQ(rig.script.setups[1].base.isa, simd::IsaPath::kScalar) << "sort has no such kernel";
  // From scalar, which dense_linear has, the worker stays: a path is changed only
  // where it must be, so that what is compared across a rotation is the same code.
  EXPECT_EQ(rig.script.setups[2].base.isa, simd::IsaPath::kScalar);
}

TEST(VerifiedRunControl, ASwitchToAWorkloadThatIsNotThereIsRefusedAndTouchesNothing) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.send({2, Switch{"no_such", 1, 1U << 20U, 0, {}}});
    }
  };
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  const std::vector<ControlAck> answers = rig.answers();
  ASSERT_EQ(answers.size(), 1U);
  EXPECT_EQ(answers[0].status, AckStatus::kRefused);
  EXPECT_EQ(answers[0].detail, "unknown workload: no workload named 'no_such'");
  EXPECT_EQ(rig.script.setups.size(), 1U) << "the runner was not replaced";
  EXPECT_EQ(rig.script.finishes, 1) << "only at the end";
}

TEST(VerifiedRunControl, ASwitchThatCannotBeMadeKeepsTheOldWorkloadRunning) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  verification::SliceReport last;
  last.next_index = 9;
  rig.script.final_report = last;
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.send({2, Switch{"sort", 1, 1, 0, {}}});  // A footprint that holds no unit.
    }
  };
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  const std::vector<ControlAck> answers = rig.answers();
  ASSERT_EQ(answers.size(), 1U);
  EXPECT_EQ(answers[0].status, AckStatus::kRefused);
  EXPECT_EQ(answers[0].detail.substr(0, 10), "workload: ") << answers[0].detail;
  ASSERT_EQ(rig.script.setups.size(), 2U);
  EXPECT_EQ(rig.script.setups[1].base.workload, "compression") << "made again, as it was";
  EXPECT_EQ(rig.script.setups[1].base.seed, 3U);
  EXPECT_EQ(rig.script.setups[1].first_index, 9U);
  EXPECT_GT(rig.script.slices_run, 1);
}

TEST(VerifiedRunControl, AWorkerThatCannotGoBackToWhatItWasRunningIsAFailureOfItsOwn) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.script.refuse_setup = [](const verification::RunnerSetup&) { return true; };
      rig.send({2, Switch{"sort", 1, std::uint64_t{8} << 20U, 0, {}}});
    }
  };
  auto outcome = rig.run();
  ASSERT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error().stage, "restore");
  EXPECT_NE(outcome.error().detail.find("could not go back to compression after refusing sort"),
            std::string::npos)
      << outcome.error().detail;
  EXPECT_NE(outcome.error().detail.find("scripted refusal"), std::string::npos);
}

TEST(VerifiedRunControl, ALeaveInTheMiddleOfAReplacementCountsTheClosedRunnerOnce) {
  // The controller is gone when the old runner's tally is sent: that runner's
  // work is in the carried tally already and is not added a second time.
  Rig rig;
  rig.script.slices = {slice_of(5)};
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.send({2, Rebind{{0}}});
    }
  };
  rig.syscalls.script_writes({{kEverything, 0}, {kEverything, 0}, {0, EPIPE}});
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value()) << describe(outcome.error());
  ASSERT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
  EXPECT_EQ(outcome.value().tally.units_computed, 5U);
  EXPECT_EQ(outcome.value().outcome.progress, 5U);
}

TEST(VerifiedRunControl, AReplacedRunnerFailingToFinishOrToSayItsLastWordEndsTheRunOrTheListening) {
  {
    Rig rig;
    rig.script.slices = {slice_of(1)};
    rig.script.finish_fails = true;
    rig.send({2, Rebind{{0}}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "runner");
    EXPECT_EQ(outcome.error().detail, "finish: scripted finish");
  }
  {
    // The memory frame goes, then the controller is gone before the old runner's tally.
    Rig rig;
    rig.send({2, Rebind{{0}}});
    rig.syscalls.script_writes({{kEverything, 0}, {0, EPIPE}});
    auto outcome = rig.run();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
    EXPECT_EQ(rig.script.setups.size(), 1U);
  }
  {
    // The old runner's tally is refused.
    Rig rig;
    rig.send({2, Rebind{{0}}});
    rig.syscalls.script_writes({{kEverything, 0}, {0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
  }
  {
    // The new runner's memory frame is refused, and then the controller is gone.
    Rig rig;
    rig.send({2, Rebind{{0}}});
    rig.syscalls.script_writes({{kEverything, 0}, {kEverything, 0}, {0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
  }
  {
    Rig rig;
    rig.send({2, Rebind{{0}}});
    rig.syscalls.script_writes({{kEverything, 0}, {kEverything, 0}, {0, EPIPE}});
    auto outcome = rig.run();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
  }
  {
    // The answer is refused.
    Rig rig;
    rig.send({2, Rebind{{0}}});
    rig.syscalls.script_writes({{kEverything, 0}, {kEverything, 0}, {kEverything, 0}, {0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
  }
}

TEST(VerifiedRunControl, AnAnswerTheControllerWillNotHearEndsTheListeningOrTheWorker) {
  {
    Rig rig;
    rig.send({1, Stop{}});
    rig.syscalls.script_writes({{kEverything, 0}, {0, EPIPE}});
    auto outcome = rig.run();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
    EXPECT_EQ(rig.script.finishes, 0);
  }
  {
    Rig rig;
    rig.send({1, Stop{}});
    rig.syscalls.script_writes({{kEverything, 0}, {0, EIO}});
    auto outcome = rig.run();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().stage, "record channel");
  }
  {
    Rig rig;
    rig.send({1, Pause{}});
    rig.syscalls.script_writes({{kEverything, 0}, {0, EPIPE}});
    auto outcome = rig.run();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
  }
}

TEST(VerifiedRunControl, APipeThatCarriesNothingLikeAMessageEndsTheWorkerAsAControllerLost) {
  Rig rig;
  rig.syscalls.deliver_to_pipe(rig.control.read_fd, std::string(24, 'x'));
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControlChannelBroken);
  EXPECT_TRUE(controller_lost(outcome.value().outcome.reason));
  EXPECT_EQ(rig.script.finishes, 0);
}

TEST(VerifiedRunControl, MessagesSentBeforeTheControllerClosedThePipeAreHandledFirst) {
  Rig rig;
  rig.send({1, Pause{}});
  ASSERT_TRUE(rig.syscalls.close(rig.control.write_fd).has_value());
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome.value().outcome.reason, StopReason::kControllerHungUp);
  EXPECT_EQ(rig.answers(), (std::vector<ControlAck>{{1, AckStatus::kDone, ""}}));
}

// A runner replaced during a run says "the whole run did not check ..." about its own
// stretch; the worker keeps only the statements that are true of the whole run.
std::vector<std::string> statements_made(Rig& rig) {
  std::vector<std::string> out;
  for (const verification::ErrorRecord& record : rig.findings()) {
    out.push_back(std::get<verification::VerificationNotPerformed>(record).what);
  }
  return out;
}

verification::SliceReport three_statements() {
  verification::SliceReport report;
  report.records.emplace_back(unrun("oracle over compression units"));
  report.records.emplace_back(unrun("input-integrity bracket over compression units"));
  report.records.emplace_back(unrun("something else"));
  return report;
}

TEST(VerifiedRunControl, ARunnerReplacedMidRunMakesNoStatementOfWhatTheWholeRunDidNotCheck) {
  Rig rig;
  rig.script.slices = {slice_of(1)};  // Every slice compares one block and brackets one unit.
  rig.script.final_report = three_statements();
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.send({1, Rebind{{0}}});
    }
  };
  ASSERT_TRUE(rig.run().has_value());
  EXPECT_EQ(statements_made(rig), (std::vector<std::string>{"something else", "something else"}))
      << "one for each runner; the two statements about the whole run are dropped, at the "
         "replacement because the run is not over and at the end because it checked";
}

TEST(VerifiedRunControl, AFindingThatIsNoStatementOfAnUnrunCheckPassesEveryReplacementUntouched) {
  Rig rig;
  rig.script.slices = {slice_of(1)};
  verification::ExactBitCorruption wrong;
  wrong.provenance.workload = "compression";
  wrong.provenance.iteration = 3;
  wrong.provenance.logical_cpu = 0;
  wrong.check = "decoded output against input";
  verification::SliceReport last;
  last.records.emplace_back(wrong);
  rig.script.final_report = last;
  rig.script.on_slice = [&rig](int slice) {
    if (slice == 1) {
      rig.send({1, Rebind{{0}}});
    }
  };
  ASSERT_TRUE(rig.run().has_value());
  std::size_t kept = 0;
  for (const verification::ErrorRecord& record : rig.findings()) {
    kept += std::holds_alternative<verification::ExactBitCorruption>(record) ? 1U : 0U;
  }
  EXPECT_EQ(kept, 2U) << "one from each runner's last report";
}

TEST(VerifiedRunControl, AWholeRunThatCheckedNothingStatesItAtTheEndAndNotBefore) {
  Rig rig;
  rig.script.final_report = three_statements();
  rig.send({1, Rebind{{0}}});  // Before any slice: the first runner ran nothing.
  rig.send({2, Pause{}});
  auto outcome = rig.run();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(statements_made(rig),
            (std::vector<std::string>{"something else", "oracle over compression units",
                                      "input-integrity bracket over compression units",
                                      "something else"}))
      << "at the replacement only what is not about the whole run; at the end all three, "
         "because nothing was compared in the whole run";
}

TEST(VerifiedRunControl, EachStatementIsKeptOrDroppedByItsOwnFigure) {
  {
    Rig rig;  // Blocks compared, nothing bracketed.
    verification::SliceReport work = slice_of(1);
    work.tally.integrity_checked = 0;
    rig.script.slices = {work};
    rig.script.final_report = three_statements();
    ASSERT_TRUE(rig.run().has_value());
    EXPECT_EQ(statements_made(rig),
              (std::vector<std::string>{"input-integrity bracket over compression units",
                                        "something else"}));
  }
  {
    Rig rig;  // Units bracketed, no block compared.
    verification::SliceReport work = slice_of(1);
    work.tally.oracle_blocks = 0;
    rig.script.slices = {work};
    rig.script.final_report = three_statements();
    ASSERT_TRUE(rig.run().has_value());
    EXPECT_EQ(statements_made(rig),
              (std::vector<std::string>{"oracle over compression units", "something else"}));
  }
}

}  // namespace
}  // namespace loadforge::worker
