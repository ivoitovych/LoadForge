// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/verified_run.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "platform/affinity.hpp"
#include "platform/clock.hpp"
#include "platform/fpenv/environment.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/oracle.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"
#include "worker/control.hpp"
#include "worker/control_channel.hpp"
#include "worker/duty.hpp"
#include "worker/entry.hpp"
#include "worker/executor.hpp"
#include "worker/executor/thread_pool.hpp"
#include "worker/frames.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"
#include "worker/services.hpp"
#include "workloads/workload.hpp"

namespace loadforge::worker {
namespace {

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
/// A tick's target is this fraction of the heartbeat interval: half.
constexpr std::int64_t kTargetDivisor = 2;
/// Never aim for less than a millisecond a slice.
constexpr std::int64_t kMinTargetNs = kNanosecondsPerMillisecond;

WorkerError error_at(std::string_view stage, std::string detail) {
  return WorkerError{std::string{stage}, std::move(detail)};
}

/// Every record of a report, then the running tally. Stops at the first frame
/// the channel will not take.
Delivery send_report(ipc::RecordWriter& records, const verification::SliceReport& report,
                     const verification::Tally& total) {
  Delivery sent;
  for (const verification::ErrorRecord& record : report.records) {
    sent = deliver(records, verification::kErrorRecordFrameType, verification::encode(record));
    if (sent.controller_gone || sent.failure.has_value()) {
      return sent;
    }
  }
  return deliver(records, kTallyFrameType, encode_tally(total));
}

/// Whether a record states what a run did not check, in the words the runner
/// contract gives its finish() (runner.hpp): "oracle over <workload> units" or
/// "input-integrity bracket over <workload> units".
bool states_zero(const verification::ErrorRecord& record, std::string_view subject) {
  const auto* unrun = std::get_if<verification::VerificationNotPerformed>(&record);
  return unrun != nullptr && unrun->what.starts_with(subject);
}

constexpr std::string_view kOracleSubject = "oracle over ";
constexpr std::string_view kBracketSubject = "input-integrity bracket over ";

/// The statements a runner's finish() makes about "the whole run", kept only
/// where they are true of the worker's whole run. A worker replaces its runner
/// on a Switch or a Rebind, and a runner that ran for a rotation may have
/// compared nothing while the worker, over all its rotations, compared a
/// great deal (the duty cycle makes some units unchecked): its zero is the
/// runner's and not the run's. So a statement is kept only at the end of the
/// run (`run_over`), and then only if the whole run's tally is zero too.
void keep_true_statements(verification::SliceReport& report, const verification::Tally& whole,
                          bool run_over) {
  const bool oracle_zero = run_over && whole.oracle_blocks == 0;
  const bool bracket_zero = run_over && whole.integrity_checked == 0;
  std::erase_if(report.records, [&](const verification::ErrorRecord& record) {
    return (states_zero(record, kOracleSubject) && !oracle_zero) ||
           (states_zero(record, kBracketSubject) && !bracket_zero);
  });
}

}  // namespace

// A size and two durations, named in the order the sentence in the header reads
// them; a struct for three numbers would be one more type for a pure function.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::uint64_t next_slice_units(std::uint64_t last_units, std::int64_t took_ns,
                               std::int64_t target_ns) noexcept {
  const std::uint64_t doubled = std::min(last_units * 2, kMaxSliceUnits);
  if (took_ns <= 0) {
    return doubled;
  }
  const auto took = static_cast<std::uint64_t>(took_ns);
  const auto target = static_cast<std::uint64_t>(std::max<std::int64_t>(target_ns, 1));
  // last_units is at most kMaxSliceUnits (2^20) and a target is a heartbeat
  // interval in nanoseconds, so the product stays far inside 64 bits.
  const std::uint64_t wanted = last_units * target / took;
  return std::clamp<std::uint64_t>(wanted, 1, doubled);
}

core::Result<std::vector<ThreadPlacement>, std::string> placements_for(
    std::uint32_t threads, const std::vector<std::uint32_t>& cpus) {
  std::vector<ThreadPlacement> placements;
  placements.reserve(threads);
  for (std::uint32_t thread = 0; thread < threads; ++thread) {
    if (cpus.empty()) {
      placements.emplace_back(std::nullopt);
      continue;
    }
    auto mask = platform::mask_of({cpus[thread % cpus.size()]});
    if (!mask.has_value()) {
      return std::string{mask.error()};
    }
    placements.emplace_back(mask.value());
  }
  return placements;
}

core::Result<Prepared, WorkerError> prepare(platform::Syscalls& syscalls, const WorkloadArgs& args,
                                            const std::vector<std::uint32_t>& cpus,
                                            std::uint64_t first_index, const Services& services) {
  // 1. Before any thread: the threads inherit it.
  if (auto established = fpenv::establish_environment(*services.fp); !established.has_value()) {
    return error_at("floating-point environment", fpenv::describe(established.error()));
  }
  // 2. The path was chosen once, by the controller. It is used or refused.
  auto path = simd::select_path(args.isa, services.isa->supported(), simd::compiled_paths());
  if (!path.has_value()) {
    return error_at("isa path", path.error().detail);
  }
  // 3. The workload, its oracle for the plan it resolved, the executor.
  const workloads::WorkloadConfig config{args.seed, args.footprint_bytes, args.decomposition,
                                         args.isa};
  auto workload = services.workloads->create(args.workload, config);
  if (!workload.has_value()) {
    return error_at("workload", std::string{workloads::describe(workload.error().fault)} + ": " +
                                    workload.error().detail);
  }
  Prepared prepared;
  prepared.workload = std::move(workload).value();
  auto oracle = services.oracles->create(args.workload, config, prepared.workload->plan());
  if (!oracle.has_value()) {
    return error_at("oracle", std::string{verification::describe(oracle.error().fault)} + ": " +
                                  oracle.error().detail);
  }
  prepared.oracle = std::move(oracle).value();
  auto placements = placements_for(args.threads, cpus);
  if (!placements.has_value()) {
    return error_at("executor", "cpus: " + placements.error());
  }
  auto pool = make_thread_pool(placements.value(), *services.threads, syscalls, *services.fp);
  if (!pool.has_value()) {
    return error_at("executor", describe(pool.error()));
  }
  prepared.executor = std::move(pool).value();
  // 4. After everything that could have loaded a library.
  if (auto asserted = fpenv::assert_environment(*services.fp); !asserted.has_value()) {
    return error_at("floating-point environment",
                    "changed during start-up: " + fpenv::describe(asserted.error()));
  }

  verification::RunnerSetup setup;
  setup.workload = prepared.workload.get();
  setup.oracle = prepared.oracle.get();
  setup.executor = prepared.executor.get();
  setup.syscalls = &syscalls;
  setup.duty = args.duty;
  setup.line_bytes = args.line_bytes;
  setup.base.workload = args.workload;
  setup.base.workload_version = prepared.workload->descriptor().version;
  setup.base.seed = args.seed;
  setup.base.isa = args.isa;
  setup.base.physical_package = args.physical_package;
  setup.base.physical_core = args.physical_core;
  setup.base.decomposition = prepared.workload->plan().decomposition;
  setup.base.footprint_bytes = args.footprint_bytes;
  setup.lock = args.lock;
  setup.base.numa_node = args.numa_node;
  if (cpus.size() == 1) {
    setup.base.logical_cpu = cpus.front();
  }
  setup.first_index = first_index;
  auto runner = services.make_runner(setup);
  if (!runner.has_value()) {
    return error_at("runner", std::string{verification::describe(runner.error().fault)} + ": " +
                                  runner.error().detail);
  }
  prepared.runner = std::move(runner).value();
  return prepared;
}

namespace {

/// What one tick came to: go on, end the run (and state what it did not
/// check), or leave at once because nobody is listening.
enum class Step : std::uint8_t { kContinue, kEnd, kLeave };

/// The loop's state across ticks, so that a tick is a function and the run is
/// a loop over it.
///
/// A tick that finds messages on the control pipe handles them, in the order
/// they came, and runs no slice: the next tick, with a fresh clock, decides
/// what the state they left calls for.
class Ticker {
 public:
  Ticker(platform::Syscalls& syscalls, ipc::RecordWriter& records, const Session& session,
         const WorkloadArgs& args, const std::vector<std::uint32_t>& cpus, const Services& services,
         Prepared prepared)
      : syscalls_(&syscalls),
        records_(&records),
        session_(&session),
        services_(&services),
        args_(args),
        cpus_(cpus),
        prepared_(std::move(prepared)),
        clock_(syscalls),
        channel_(syscalls, session.control_fd, records),
        pid_(static_cast<std::uint32_t>(syscalls.process_id())),
        interval_ms_(static_cast<int>(session.pacing.interval_ns / kNanosecondsPerMillisecond)),
        target_ns_(std::max(session.pacing.interval_ns / kTargetDivisor, kMinTargetNs)),
        next_index_(0) {}

  /// Reads the clock for the first time and arms the watch on the controller.
  [[nodiscard]] std::optional<WorkerError> start() {
    auto started = clock_.now();
    if (!started.has_value()) {
      return error_at("clock", describe(started.error()));
    }
    started_ = started.value();
    watch_ = std::make_unique<ControllerWatch>(*syscalls_, *session_, started_.running_ns);
    return std::nullopt;
  }

  /// What was done about locking the working set, told before the first slice
  /// and again whenever the runner is replaced.
  [[nodiscard]] Delivery tell_memory() {
    return deliver(*records_, kMemoryFrameType, encode_memory(prepared_.runner->memory_lock()));
  }

  /// One tick: the beat, then the judgements, then the control pipe, then a slice.
  [[nodiscard]] core::Result<Step, WorkerError> once() {
    auto now = clock_.now();
    if (!now.has_value()) {
      return error_at("clock", describe(now.error()));
    }
    auto elapsed = platform::Clock::between(started_, now.value());
    if (!elapsed.has_value()) {
      return error_at("clock", describe(elapsed.error()));
    }
    const std::int64_t elapsed_ns = elapsed.value().raw_ns;
    result_.outcome.elapsed_ns = elapsed_ns;
    result_.outcome.progress = whole().units_computed;

    const ipc::Heartbeat beat{now.value().running_ns, result_.outcome.progress,
                              paused_ ? NullWorkload::kPhasePaused : NullWorkload::kPhaseRunning,
                              pid_};
    if (auto published = session_->region->publish(ipc::worker_slot(session_->slot), beat);
        !published.has_value()) {
      return error_at("heartbeat", describe(published.error()));
    }
    if (elapsed_ns >= session_->pacing.duration_ns) {
      return end(StopReason::kDurationElapsed);
    }
    if (const auto gone = watch_->gone(now.value(), elapsed_ns); gone.has_value()) {
      return leave(*gone);
    }
    // A worker with work to do has nothing to wait for and only looks at its
    // control pipe; one that is paused, or in the idle part of its duty cycle,
    // waits on it, and so hears a message at once.
    const DutyClock::Position position = duty_.at(now.value().running_ns);
    const bool idle = paused_ || !position.busy;
    auto looked = channel_.look(idle ? wait_ms(position) : 0);
    if (!looked.has_value()) {
      return looked.error();
    }
    for (const ControlMessage& message : looked.value().messages) {
      auto handled = handle(message, now.value());
      if (!handled.has_value() || handled.value() != Step::kContinue) {
        return handled;
      }
    }
    if (looked.value().lost.has_value()) {
      return leave(*looked.value().lost);
    }
    if (!looked.value().messages.empty() || idle) {
      return Step::kContinue;
    }
    return slice(now.value(), position.remaining_ns);
  }

  /// The run's last word: what the whole run did not check, then the tally.
  [[nodiscard]] core::Result<VerifiedOutcome, WorkerError> conclude() {
    auto closed = close_runner(true);
    if (!closed.has_value()) {
      return closed.error();
    }
    result_.tally = carried_;
    result_.outcome.progress = carried_.units_computed;
    // A controller that went while the last word was sent has already set the
    // reason (after()); otherwise it is the one the run ended for.
    return result_;
  }

  [[nodiscard]] const VerifiedOutcome& result() const noexcept { return result_; }

 private:
  /// Everything counted so far: the runners that were replaced, and this one.
  [[nodiscard]] verification::Tally whole() const { return carried_ + prepared_.runner->total(); }

  /// How long to wait on the control pipe while there is nothing to run: a
  /// paused worker, one interval; one in the idle part of a duty cycle, until
  /// that part ends (rounded up, so the wait is never zero), at most one
  /// interval so that it keeps beating.
  [[nodiscard]] int wait_ms(const DutyClock::Position& position) const {
    if (paused_) {
      return interval_ms_;
    }
    const std::int64_t until =
        (position.remaining_ns + kNanosecondsPerMillisecond - 1) / kNanosecondsPerMillisecond;
    return static_cast<int>(std::clamp<std::int64_t>(until, 1, interval_ms_));
  }

  Step end(StopReason reason) {
    result_.outcome.reason = reason;
    return Step::kEnd;
  }
  Step leave(StopReason reason) {
    result_.outcome.reason = reason;
    return Step::kLeave;
  }

  /// What a frame that was sent comes to for the loop.
  core::Result<Step, WorkerError> after(const Delivery& sent) {
    if (sent.failure.has_value()) {
      return *sent.failure;
    }
    if (sent.controller_gone) {
      return leave(StopReason::kControllerHungUp);
    }
    return Step::kContinue;
  }

  core::Result<Step, WorkerError> reply(std::uint32_t sequence, AckStatus status,
                                        std::string detail) {
    return after(channel_.answer(sequence, status, std::move(detail)));
  }

  core::Result<Step, WorkerError> handle(const ControlMessage& message,
                                         const platform::TimePoint& now) {
    return std::visit([&](const auto& command) { return apply(message.sequence, command, now); },
                      message.command);
  }

  // The six commands (worker/control.hpp), each answered once.
  core::Result<Step, WorkerError> apply(std::uint32_t sequence, const Stop& /*stop*/,
                                        const platform::TimePoint& /*now*/) {
    auto replied = reply(sequence, AckStatus::kDone, "");
    if (!replied.has_value() || replied.value() != Step::kContinue) {
      return replied;
    }
    return end(StopReason::kStopRequested);
  }

  core::Result<Step, WorkerError> apply(std::uint32_t sequence, const Pause& /*pause*/,
                                        const platform::TimePoint& /*now*/) {
    paused_ = true;
    return reply(sequence, AckStatus::kDone, "");
  }

  core::Result<Step, WorkerError> apply(std::uint32_t sequence, const Resume& /*resume*/,
                                        const platform::TimePoint& /*now*/) {
    paused_ = false;
    return reply(sequence, AckStatus::kDone, "");
  }

  core::Result<Step, WorkerError> apply(std::uint32_t sequence, const SetDuty& duty,
                                        const platform::TimePoint& now) {
    duty_.set(duty, now.running_ns);
    return reply(sequence, AckStatus::kDone, "");
  }

  core::Result<Step, WorkerError> apply(std::uint32_t sequence, const Rebind& move,
                                        const platform::TimePoint& /*now*/) {
    // Where the worker runs is no longer a fact the controller took from the
    // topology: the records say nothing of the package, the core or the node
    // rather than something that was true before the move.
    WorkloadArgs next = args_;
    next.physical_package.reset();
    next.physical_core.reset();
    next.numa_node.reset();
    return reconfigure(sequence, next, move.cpus);
  }

  core::Result<Step, WorkerError> apply(std::uint32_t sequence, const Switch& change,
                                        const platform::TimePoint& /*now*/) {
    auto described = services_->workloads->descriptor(change.workload);
    if (!described.has_value()) {
      // Nothing has been touched: the old workload goes on.
      return reply(
          sequence, AckStatus::kRefused,
          refusal_words(workloads::describe(described.error().fault), described.error().detail));
    }
    WorkloadArgs next = args_;
    next.workload = change.workload;
    next.seed = change.seed;
    next.footprint_bytes = change.footprint_bytes;
    next.decomposition = change.decomposition;
    next.duty = change.duty;
    // The path the worker is on, where the new workload has it; else the best
    // both the machine and the new workload offer, as the controller chose
    // for the first (plan.hpp). Scalar is always there, so this cannot fail.
    const simd::IsaSet reachable = simd::compiled_paths().intersect(described.value().paths);
    const std::optional<simd::IsaPath> keep =
        reachable.contains(args_.isa) ? std::optional<simd::IsaPath>{args_.isa} : std::nullopt;
    next.isa = simd::select_path(keep, services_->isa->supported(), reachable).value();
    return reconfigure(sequence, next, cpus_);
  }

  static std::string refusal_words(std::string_view what, const std::string& detail) {
    std::string text{what};
    text += ": ";
    text += detail;
    return text;
  }

  /// The runner's last word, and its tally into the worker's: the records of a
  /// runner that finishes are sent before anything else is said, but a
  /// statement about "the whole run" is the worker's to make, not a runner's
  /// that lived for a rotation (keep_true_statements).
  core::Result<Step, WorkerError> close_runner(bool run_over) {
    verification::SliceRunner& runner = *prepared_.runner;
    auto last = runner.finish();
    if (!last.has_value()) {
      // finish() answers kFinished only to a second call, and this is the first.
      return error_at("runner", "finish: " + last.error().detail);
    }
    carried_ = carried_ + runner.total();
    next_index_ = last.value().next_index;
    verification::SliceReport report = std::move(last).value();
    keep_true_statements(report, carried_, run_over);
    return after(send_report(*records_, report, carried_));
  }

  /// Frees what the runner stood on, last made first.
  void release() {
    prepared_.runner.reset();
    prepared_.executor.reset();
    prepared_.oracle.reset();
    prepared_.workload.reset();
  }

  /// Replaces the runner: `next` on `cpus`, from the next unit. The old
  /// runner's records go out first; its memory is freed before the new one is
  /// made (a rotation must not need twice the footprint); and if the new one
  /// cannot be made the old is made again and the command refused with the
  /// reason, so that a refused command costs a pause and not the run.
  core::Result<Step, WorkerError> reconfigure(std::uint32_t sequence, const WorkloadArgs& next,
                                              const std::vector<std::uint32_t>& cpus) {
    auto closed = close_runner(false);
    if (!closed.has_value() || closed.value() != Step::kContinue) {
      return closed;
    }
    release();
    std::string refusal;
    auto fresh = prepare(*syscalls_, next, cpus, next_index_, *services_);
    if (fresh.has_value()) {
      prepared_ = std::move(fresh).value();
      args_ = next;
      cpus_ = cpus;
      units_ = 1;
    } else {
      refusal = describe(fresh.error());
      auto restored = prepare(*syscalls_, args_, cpus_, next_index_, *services_);
      if (!restored.has_value()) {
        std::string lost = "could not go back to ";
        lost += args_.workload;
        lost += " after refusing ";
        lost += next.workload;
        lost += " (";
        lost += refusal;
        lost += ")";
        return error_at("restore", lost + ": " + describe(restored.error()));
      }
      prepared_ = std::move(restored).value();
      units_ = 1;
    }
    auto told = after(tell_memory());
    if (!told.has_value() || told.value() != Step::kContinue) {
      return told;
    }
    if (refusal.empty()) {
      return reply(sequence, AckStatus::kDone, "");
    }
    return reply(sequence, AckStatus::kRefused, std::move(refusal));
  }

  core::Result<Step, WorkerError> slice(const platform::TimePoint& now,
                                        std::int64_t busy_remaining_ns) {
    auto ran = prepared_.runner->run_slice(verification::SliceRequest{units_});
    if (!ran.has_value()) {
      // kFinished is the only error a slice has: the runner ended the run
      // itself (a slice before this one said why, in a record).
      result_.note = ran.error().detail;
      return end(StopReason::kRunnerStopped);
    }
    next_index_ = ran.value().next_index;
    const Delivery sent = send_report(*records_, ran.value(), whole());
    if (sent.failure.has_value()) {
      return *sent.failure;
    }
    if (sent.controller_gone) {
      return leave(StopReason::kControllerHungUp);
    }
    auto after = clock_.now();
    if (!after.has_value()) {
      return error_at("clock", describe(after.error()));
    }
    // A slice is aimed to end with the busy part of the duty cycle, but never
    // shorter than the least a slice is aimed at.
    const std::int64_t aim = std::max(std::min(target_ns_, busy_remaining_ns), kMinTargetNs);
    units_ = next_slice_units(units_, after.value().running_ns - now.running_ns, aim);
    return Step::kContinue;
  }

  platform::Syscalls* syscalls_;
  ipc::RecordWriter* records_;
  const Session* session_;
  const Services* services_;
  WorkloadArgs args_;
  std::vector<std::uint32_t> cpus_;
  Prepared prepared_;
  platform::Clock clock_;
  ControlChannel channel_;
  std::uint32_t pid_;
  int interval_ms_;
  std::int64_t target_ns_;
  platform::TimePoint started_;
  std::unique_ptr<ControllerWatch> watch_;
  std::uint64_t units_ = 1;
  std::uint64_t next_index_;
  bool paused_ = false;
  DutyClock duty_;
  verification::Tally carried_;
  VerifiedOutcome result_;
};

}  // namespace

core::Result<VerifiedOutcome, WorkerError> VerifiedRun::run() {
  auto prepared = prepare(*syscalls_, *workload_, args_->cpus, 0, *services_);
  if (!prepared.has_value()) {
    return prepared.error();
  }
  Ticker ticker(*syscalls_, *records_, session_, *workload_, args_->cpus, *services_,
                std::move(prepared).value());
  // What was done about locking is told once, before the first slice.
  const Delivery told = ticker.tell_memory();
  if (told.failure.has_value()) {
    return *told.failure;
  }
  if (told.controller_gone) {
    VerifiedOutcome gone;
    gone.outcome.reason = StopReason::kControllerHungUp;
    return gone;
  }
  if (auto failed = ticker.start(); failed.has_value()) {
    return *failed;
  }
  while (true) {
    auto step = ticker.once();
    if (!step.has_value()) {
      return step.error();
    }
    if (step.value() == Step::kLeave) {
      return ticker.result();
    }
    if (step.value() == Step::kEnd) {
      // Ended by time, by request, or by the runner: its last word goes out
      // before the worker says it finished.
      return ticker.conclude();
    }
  }
}

}  // namespace loadforge::worker
