// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/orchestration.hpp"

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "config/config.hpp"
#include "controller/plan.hpp"
#include "controller/report.hpp"
#include "controller/safety.hpp"
#include "controller/supervision.hpp"
#include "core/result.hpp"
#include "platform/clock.hpp"
#include "platform/exit_status.hpp"
#include "platform/fpenv/environment.hpp"
#include "platform/process.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "platform/worker_pool.hpp"
#include "resources/oom_kill_record.hpp"
#include "telemetry/capability/capability.hpp"
#include "telemetry/timeline.hpp"
#include "topology/snapshot.hpp"
#include "verification/record.hpp"
#include "worker/entry.hpp"
#include "worker/frames.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"

namespace loadforge::controller {

std::size_t clean_count(const RunReport& report) noexcept {
  std::size_t clean = 0;
  for (const WorkerReport& worker : report.workers) {
    if (is_clean(worker.fate)) {
      ++clean;
    }
  }
  return clean;
}

bool all_clean(const RunReport& report) noexcept {
  return !report.workers.empty() && clean_count(report) == report.workers.size();
}

std::uint64_t wrong_result_count(const RunReport& report) noexcept {
  std::uint64_t total = 0;
  for (const WorkerReport& worker : report.workers) {
    total += worker.wrong_results;
  }
  return total;
}

std::uint64_t not_performed_count(const RunReport& report) noexcept {
  std::uint64_t total = 0;
  for (const WorkerReport& worker : report.workers) {
    total += worker.not_performed;
  }
  return total;
}

bool operator==(const WorkerWork& lhs, const WorkerWork& rhs) {
  const auto what = [](const WorkerWork& w) {
    return std::tie(w.descriptor, w.isa, w.decomposition, w.duty, w.logical_bytes);
  };
  const auto where = [](const WorkerWork& w) {
    return std::tie(w.threads, w.cpu, w.seed, w.footprint_bytes, w.line_bytes);
  };
  return what(lhs) == what(rhs) && where(lhs) == where(rhs);
}

bool operator==(const FindingReport& lhs, const FindingReport& rhs) {
  return std::tie(lhs.record, lhs.isolation, lhs.not_isolated) ==
         std::tie(rhs.record, rhs.isolation, rhs.not_isolated);
}

std::string describe(const ControllerError& error) { return error.stage + ": " + error.detail; }

bool operator==(const Reap& lhs, const Reap& rhs) noexcept {
  return std::tie(lhs.slot, lhs.at_ns) == std::tie(rhs.slot, rhs.at_ns);
}

bool operator==(const SamplingReport& lhs, const SamplingReport& rhs) noexcept {
  return std::tie(lhs.enabled, lhs.interval_ns, lhs.rounds, lhs.points, lhs.cost_ns,
                  lhs.first_round_ns, lhs.last_round_ns, lhs.reaps, lhs.fault) ==
         std::tie(rhs.enabled, rhs.interval_ns, rhs.rounds, rhs.points, rhs.cost_ns,
                  rhs.first_round_ns, rhs.last_round_ns, rhs.reaps, rhs.fault);
}

namespace {

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
constexpr std::uint32_t kControllerPhase = 1;
/// How many reads one tick gives a channel that keeps having more: a worker
/// with a persistent fault makes a record a unit, and one read a tick would
/// let its pipe fill, block it in write, and have it judged a hang by the very
/// controller that was not reading. Bounded, so that a channel which never
/// stops cannot hold the loop (and every other worker's beat) hostage.
constexpr int kMaxReadsPerTick = 64;
/// How many drains a channel is given after its worker is dead before the
/// controller concludes that something else holds the write end open.
constexpr int kMaxDrainsAfterDeath = 1024;

/// What a refused round or phase says: the fault's words, and the timeline's
/// own detail in brackets when it gave one.
std::string describe_refusal(const telemetry::TimelineError& error) {
  std::string text{telemetry::describe(error.fault)};
  if (!error.detail.empty()) {
    text += " (";
    text += error.detail;
    text += ')';
  }
  return text;
}

ControllerError error_at(std::string_view stage, std::string detail) {
  return ControllerError{std::string{stage}, std::move(detail)};
}

/// Whether a poll answer means the descriptor should be read: bytes, or any
/// of the three ways the far side can be gone.
bool wakes(const platform::Readiness& readiness) {
  return readiness.readable || readiness.hung_up || readiness.error || readiness.invalid;
}

std::string text_of(const std::vector<std::byte>& payload) {
  std::string text;
  text.reserve(payload.size());
  for (const std::byte byte : payload) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return text;
}

std::string_view record_word(std::uint16_t type) {
  switch (static_cast<ipc::RecordType>(type)) {
    case ipc::RecordType::kStarted:
    case ipc::RecordType::kFinished:
    case ipc::RecordType::kError:
      return ipc::describe(static_cast<ipc::RecordType>(type));
  }
  return "unknown record";
}

/// Everything the controller holds about one worker while it runs.
struct Tracked {
  std::size_t slot = 0;
  pid_t pid = 0;
  platform::PipeEnds records;  ///< The controller reads; the worker writes.
  platform::PipeEnds control;  ///< The controller writes; the worker reads.
  std::unique_ptr<ipc::RecordSource> source;
  bool channel_open = false;
  StopStep step = StopStep::kNone;
  std::int64_t step_since_ns = 0;
  Evidence evidence;
  bool reaped = false;
  std::uint64_t progress = 0;
  std::int64_t progress_changed_ns = 0;
  std::int64_t launched_ns = 0;   ///< The controller's clock when this worker was forked.
  std::int64_t last_read_ns = 0;  ///< When its slot last gave a consistent sample.
  std::vector<std::string> notes;
  WorkerPlan plan;  ///< What it was told to run (null workload: no args).
  verification::Tally tally;
  bool tally_seen = false;
  std::vector<FindingReport> findings;
  std::uint64_t wrong_results = 0;
  std::uint64_t not_performed = 0;
  std::uint64_t findings_dropped = 0;
};

/// One run, from the first beat to the report. Holds what must be released
/// however the run ends; the destructor does the releasing silently and
/// `finish` does it aloud.
class Run {
 public:
  Run(platform::Syscalls& syscalls, Reporter& reporter, const config::Config& config,
      const RunOptions& options)
      : syscalls_(&syscalls),
        reporter_(&reporter),
        config_(&config),
        options_(&options),
        clock_(syscalls),
        launcher_(syscalls, options.executable),
        pool_(syscalls, launcher_) {}

  ~Run() { teardown(); }
  Run(const Run&) = delete;
  Run(Run&&) = delete;
  Run& operator=(const Run&) = delete;
  Run& operator=(Run&&) = delete;

  core::Result<RunReport, ControllerError> execute();

 private:
  [[nodiscard]] core::Result<core::Ok, ControllerError> prepare();
  [[nodiscard]] core::Result<std::optional<int>, ControllerError> spawn();
  [[nodiscard]] core::Result<core::Ok, ControllerError> tick();
  void beat(const platform::TimePoint& now);
  [[nodiscard]] core::Result<core::Ok, ControllerError> request_stop(std::int64_t now_ns);
  [[nodiscard]] core::Result<core::Ok, ControllerError> watch(Tracked& worker,
                                                              const platform::TimePoint& now);
  [[nodiscard]] core::Result<core::Ok, ControllerError> send(Tracked& worker, int signal,
                                                             std::int64_t now_ns, bool stale);
  [[nodiscard]] core::Result<core::Ok, ControllerError> poll_and_drain(int timeout_ms);
  [[nodiscard]] core::Result<core::Ok, ControllerError> drain(Tracked& worker);
  [[nodiscard]] core::Result<core::Ok, ControllerError> drain_available(Tracked& worker);
  /// Takes one record a worker sent into the worker's account of itself, or
  /// says why the channel can no longer be believed.
  [[nodiscard]] static std::optional<std::string> ingest(Tracked& worker,
                                                         const ipc::Record& record);
  [[nodiscard]] static std::optional<std::string> take_error_record(Tracked& worker,
                                                                    const ipc::Record& record);
  [[nodiscard]] static std::optional<std::string> take_tally(Tracked& worker,
                                                             const ipc::Record& record);
  [[nodiscard]] core::Result<core::Ok, ControllerError> reap(std::int64_t elapsed_ns);
  [[nodiscard]] core::Result<core::Ok, ControllerError> drain_after_death();
  void attribute_oom_kills();
  void begin_telemetry();
  void take_round(std::int64_t now_ns, bool force);
  void mark_phase(std::string_view label, std::int64_t now_ns);
  void telemetry_fault(const std::string& text, std::int64_t now_ns);
  [[nodiscard]] core::Result<RunReport, ControllerError> finish();
  void close_channel(Tracked& worker);
  void kill_group(Tracked& worker);
  void teardown();
  [[nodiscard]] core::Result<std::int64_t, ControllerError> elapsed_now();

  platform::Syscalls* syscalls_;
  Reporter* reporter_;
  const config::Config* config_;
  const RunOptions* options_;
  platform::Clock clock_;
  platform::WorkerLauncher launcher_;
  platform::WorkerPool pool_;
  std::unique_ptr<ipc::HeartbeatRegion> region_;
  std::vector<Tracked> workers_;
  platform::TimePoint started_;
  std::int64_t duration_ns_ = 0;
  std::uint64_t beats_ = 0;
  /// The OOM-kill count before the first fork, when a record is consulted.
  /// Read in prepare whenever there is a record, so the initial words are
  /// never a reading anyone sees.
  core::Result<std::uint64_t, std::string> oom_before_{std::string{"not read"}};
  std::size_t live_ = 0;
  bool stop_requested_ = false;
  bool spawned_ = false;
  std::string environment_;
  /// What the sampler did; `sampling_live_` is false before it begins and
  /// after a round it refused.
  SamplingReport sampling_;
  bool sampling_live_ = false;
  /// The monotonic time of the latest clock reading the loop made, so that the
  /// sampling and the reaping use the reading the tick already took and add no
  /// read of their own.
  std::int64_t clock_running_ns_ = 0;
};

core::Result<core::Ok, ControllerError> Run::prepare() {
  // The controller writes to pipes whose readers can die at any moment: a
  // worker that exits between one tick's reap and the next tick's stop byte
  // has closed the read end of its control pipe. With SIGPIPE at its default,
  // that write kills the controller by signal 13 before write(2) can return
  // EPIPE -- the one answer request_stop already tolerates -- and the whole
  // run ends unreported. Ignored, the write reports EPIPE and the worker's
  // death is reported like any other. First, before anything is created, so
  // that a refusal leaves nothing to tear down. Workers inherit SIG_IGN across
  // exec; they set it themselves regardless.
  if (auto ignored = syscalls_->ignore_signal(SIGPIPE); !ignored.has_value()) {
    return error_at("signals", describe(ignored.error()));
  }
  if (auto memory = check_memory(*config_, options_->budget); !memory.has_value()) {
    return error_at("memory", memory.error());
  }
  if (options_->fp != nullptr) {
    // Before anything is planned or started: a machine whose registers are
    // not in the required state is a configuration error, reported here and
    // not as a verification failure of some worker later (determinism.md §3).
    fpenv::FpControl& control = *options_->fp;
    if (auto established = fpenv::establish_environment(control); !established.has_value()) {
      return error_at("floating-point environment", fpenv::describe(established.error()));
    }
    environment_ = "floating point (asserted at startup): " + describe_state(control.read());
  }
  for (const config::Workload& workload : config_->workloads) {
    if (workload.enabled) {
      workers_.emplace_back();
      workers_.back().slot = workers_.size() - 1;
      auto planned = plan_worker(workload, workers_.back().slot, options_->budget,
                                 options_->verification, options_->topology);
      if (!planned.has_value()) {
        return error_at("workload", planned.error());
      }
      workers_.back().plan = std::move(planned).value();
    }
  }
  if (workers_.empty()) {
    return error_at("config", "no workload is enabled");
  }
  // Before the first fork, so that every kill of a worker comes after it. A
  // count that cannot be read is not a reason to refuse the run: it is kept,
  // and said, if a worker's death ever needs it.
  if (options_->oom_record != nullptr) {
    oom_before_ = options_->oom_record->kills();
  }
  duration_ns_ = config_->run.duration.count() * kNanosecondsPerMillisecond;

  auto region = ipc::HeartbeatRegion::create(*syscalls_, workers_.size());
  if (!region.has_value()) {
    return error_at("heartbeat", describe(region.error()));
  }
  region_ = std::make_unique<ipc::HeartbeatRegion>(std::move(region).value());

  auto now = clock_.now();
  if (!now.has_value()) {
    return error_at("clock", describe(now.error()));
  }
  started_ = now.value();
  clock_running_ns_ = started_.running_ns;
  beat(started_);
  begin_telemetry();

  for (Tracked& worker : workers_) {
    auto records = syscalls_->create_pipe();
    if (!records.has_value()) {
      return error_at("record channel", describe(records.error()));
    }
    worker.records = records.value();
    auto control = syscalls_->create_pipe();
    if (!control.has_value()) {
      return error_at("control pipe", describe(control.error()));
    }
    worker.control = control.value();
  }
  return core::Ok{};
}

void Run::beat(const platform::TimePoint& now) {
  const ipc::Heartbeat beat{now.running_ns, ++beats_, kControllerPhase,
                            static_cast<std::uint32_t>(syscalls_->process_id())};
  // publish refuses only a slot the region does not have, and slot 0 is in
  // every region, so its answer carries no information here: a branch on it
  // would be one no test could take.
  (void)region_->publish(ipc::kControllerSlot, beat);
}

core::Result<std::optional<int>, ControllerError> Run::spawn() {
  const pid_t supervisor = syscalls_->process_id();
  std::vector<std::vector<std::string>> argvs;
  argvs.reserve(workers_.size());
  for (const Tracked& worker : workers_) {
    worker::WorkerArgs args;
    args.slot = worker.slot;
    args.workers = workers_.size();
    args.heartbeat_fd = region_->fd();
    args.record_fd = worker.records.write_fd;
    args.control_fd = worker.control.read_fd;
    args.supervisor_pid = supervisor;
    args.pacing = worker::Pacing{duration_ns_, options_->interval_ns, options_->stale_after_ns};
    args.cpus = worker.plan.cpus;
    args.workload = worker.plan.args;
    argvs.push_back(worker::argv_for(options_->executable, args));
  }

  // Between forks the controller beats, and notes when each worker began. A
  // spawn can take longer than the staleness allowance on a loaded machine;
  // the workers already running watch slot 0 meanwhile, and a frozen beat
  // would have them leave as "controller lost" while it is alive and forking
  // (journal §2.33). The last launch is also when the run begins: `started_`
  // is overwritten by each, so it ends as the last worker's.
  std::optional<ControllerError> clock_failure;
  const auto launched = [this, &clock_failure](
                            std::size_t slot,
                            pid_t) -> core::Result<core::Ok, platform::SyscallError> {
    auto now = clock_.now();
    if (!now.has_value()) {
      clock_failure = error_at("clock", describe(now.error()));
      return now.error();
    }
    beat(now.value());
    workers_[slot].launched_ns = now.value().running_ns;
    started_ = now.value();
    return core::Ok{};
  };
  auto spawned = pool_.spawn(argvs, launched);
  if (!spawned.has_value()) {
    if (clock_failure.has_value()) {
      return *clock_failure;
    }
    return error_at("spawn", describe(spawned.error()));
  }
  if (spawned.value().is_worker()) {
    // This process is a worker whose image is about to be replaced. If it is
    // not, the code says why, and the caller exits with it.
    const Tracked& self = workers_[spawned.value().slot()];
    const platform::WorkerStartupFailure failure = launcher_.become_worker(
        supervisor, argvs[self.slot], {region_->fd(), self.records.write_fd, self.control.read_fd});
    return std::optional<int>{static_cast<int>(failure)};
  }
  spawned_ = true;
  live_ = workers_.size();
  for (Tracked& worker : workers_) {
    worker.pid = spawned.value().pids()[worker.slot];
    worker.progress_changed_ns = worker.launched_ns;
    worker.last_read_ns = worker.launched_ns;
    // The worker's ends are the worker's now; the controller keeping a copy
    // would hide the end of file that is the one signal a dead worker cannot
    // fake.
    auto closed_records = syscalls_->close(worker.records.write_fd);
    auto closed_control = syscalls_->close(worker.control.read_fd);
    worker.records.write_fd = -1;
    worker.control.read_fd = -1;
    if (!closed_records.has_value()) {
      return error_at("spawn", describe(closed_records.error()));
    }
    if (!closed_control.has_value()) {
      return error_at("spawn", describe(closed_control.error()));
    }
    worker.source = std::make_unique<ipc::RecordSource>(*syscalls_, worker.records.read_fd);
    worker.channel_open = true;
  }
  std::string line = "spawned " + std::to_string(workers_.size()) + " worker(s): pids";
  for (const Tracked& worker : workers_) {
    line += " " + std::to_string(worker.pid);
  }
  reporter_->event(0, line);
  return std::optional<int>{};
}

core::Result<std::int64_t, ControllerError> Run::elapsed_now() {
  auto now = clock_.now();
  if (!now.has_value()) {
    return error_at("clock", describe(now.error()));
  }
  auto elapsed = platform::Clock::between(started_, now.value());
  if (!elapsed.has_value()) {
    return error_at("clock", describe(elapsed.error()));
  }
  clock_running_ns_ = now.value().running_ns;
  return elapsed.value().raw_ns;
}

core::Result<core::Ok, ControllerError> Run::tick() {
  auto now = clock_.now();
  if (!now.has_value()) {
    return error_at("clock", describe(now.error()));
  }
  auto elapsed = platform::Clock::between(started_, now.value());
  if (!elapsed.has_value()) {
    return error_at("clock", describe(elapsed.error()));
  }
  const std::int64_t elapsed_ns = elapsed.value().raw_ns;
  clock_running_ns_ = now.value().running_ns;
  beat(now.value());
  if (!stop_requested_ && elapsed_ns >= duration_ns_) {
    // The round that closes the phase the deadline ends, and the mark that
    // opens the next, share this instant: a counter's advance up to the
    // deadline then belongs to "run" and what follows to "stop".
    take_round(now.value().running_ns, true);
    mark_phase("stop", now.value().running_ns);
    if (auto requested = request_stop(now.value().running_ns); !requested.has_value()) {
      return requested;
    }
    reporter_->event(elapsed_ns, "deadline reached: stop requested");
  }
  take_round(now.value().running_ns, false);
  for (Tracked& worker : workers_) {
    if (!worker.reaped) {
      if (auto watched = watch(worker, now.value()); !watched.has_value()) {
        return watched;
      }
    }
  }
  const auto timeout_ms = static_cast<int>(options_->interval_ns / kNanosecondsPerMillisecond);
  if (auto drained = poll_and_drain(timeout_ms); !drained.has_value()) {
    return drained;
  }
  return reap(elapsed_ns);
}

core::Result<core::Ok, ControllerError> Run::request_stop(std::int64_t now_ns) {
  stop_requested_ = true;
  const std::array<std::byte, 1> stop{std::byte{'s'}};
  for (Tracked& worker : workers_) {
    if (worker.reaped || worker.step != StopStep::kNone) {
      continue;
    }
    // EPIPE means the worker is already gone and about to be reaped; any
    // other refusal is the controller's own pipe failing.
    auto written = syscalls_->write(worker.control.write_fd, stop);
    if (!written.has_value() && written.error().number != EPIPE) {
      return error_at("control pipe", describe(written.error()));
    }
    worker.step = StopStep::kRequested;
    worker.step_since_ns = now_ns;
  }
  return core::Ok{};
}

core::Result<core::Ok, ControllerError> Run::watch(Tracked& worker,
                                                   const platform::TimePoint& now) {
  // The worker's beat: stale by timestamp is the hang; fresh but with no
  // progress for the same allowance is the spin. Both are the same decision.
  //
  // A slot that cannot be read this tick has a writer in the middle of a
  // publish. That is a worker that died there and will never beat again, or
  // one the scheduler took off the core between the two halves of its write,
  // for as long as the scheduler likes: under TSan with every core busy, the
  // first version, which called one torn read a death, killed a healthy
  // worker in a 600 ms run. One look cannot tell the two apart; the allowance
  // can, so the slot is stale once nothing could be read from it for longer
  // than a beat may be old.
  bool stale = false;
  auto observed = region_->observe(ipc::worker_slot(worker.slot));
  if (!observed.has_value()) {
    stale = now.running_ns - worker.last_read_ns > options_->stale_after_ns;
  } else {
    worker.last_read_ns = now.running_ns;
    const std::optional<ipc::Heartbeat> beat = observed.value();
    const ipc::Liveness liveness = ipc::liveness(beat, now.running_ns, options_->stale_after_ns);
    if (beat.has_value() && beat->progress > worker.progress) {
      worker.progress = beat->progress;
      worker.progress_changed_ns = now.running_ns;
    }
    const bool stalled = now.running_ns - worker.progress_changed_ns > options_->stale_after_ns;
    if (liveness == ipc::Liveness::kStale) {
      stale = true;
    } else if (liveness == ipc::Liveness::kNeverBeat) {
      stale = now.running_ns - worker.launched_ns > options_->stale_after_ns;
    } else {
      stale = stalled;
    }
  }
  if (stale && (worker.step == StopStep::kNone || worker.step == StopStep::kRequested)) {
    return send(worker, SIGTERM, now.running_ns, true);
  }
  if (const auto due =
          escalation_due(worker.step, worker.step_since_ns, now.running_ns, options_->grace);
      due.has_value()) {
    return send(worker, *due, now.running_ns, worker.evidence.stale_when_signalled);
  }
  return core::Ok{};
}

core::Result<core::Ok, ControllerError> Run::send(Tracked& worker, int signal, std::int64_t now_ns,
                                                  bool stale) {
  // kill(2) succeeds on a zombie, so a success proves a signal was queued and
  // nothing about what it did (journal §1.27). A worker that has already
  // exited is therefore never signalled, and no signal is recorded for it:
  // the reap that follows says how it died, and the record must not say the
  // controller did it. The look is waitid with WNOWAIT, which reaps nothing.
  auto exited = syscalls_->has_exited(worker.pid);
  if (!exited.has_value()) {
    return error_at("signal", describe(exited.error()));
  }
  if (exited.value()) {
    worker.notes.push_back("signal " + std::to_string(signal) +
                           " not sent: the worker had already exited");
    return core::Ok{};
  }
  // What remains is a race the interface cannot close: the worker can die
  // between that look and this kill. ESRCH cannot happen to a child that is
  // unreaped, but if it does the worker is gone, which is not an error and
  // not a signal sent.
  auto sent = syscalls_->send_signal(worker.pid, signal);
  if (!sent.has_value()) {
    if (sent.error().number != ESRCH) {
      return error_at("signal", describe(sent.error()));
    }
    return core::Ok{};
  }
  worker.step = step_after(signal);
  worker.step_since_ns = now_ns;
  worker.evidence.signal_attempted = signal;
  worker.evidence.stale_when_signalled = stale;
  reporter_->event(now_ns - started_.running_ns, "worker " + std::to_string(worker.slot) +
                                                     ": sent signal " + std::to_string(signal) +
                                                     (stale ? " (heartbeat stale)" : ""));
  return core::Ok{};
}

core::Result<core::Ok, ControllerError> Run::poll_and_drain(int timeout_ms) {
  std::vector<int> fds;
  std::vector<Tracked*> watched;
  for (Tracked& worker : workers_) {
    if (worker.channel_open) {
      fds.push_back(worker.records.read_fd);
      watched.push_back(&worker);
    }
  }
  auto ready = syscalls_->poll_readable(fds, timeout_ms);
  if (!ready.has_value()) {
    return error_at("record channel", describe(ready.error()));
  }
  for (std::size_t index = 0; index < watched.size(); ++index) {
    const platform::Readiness& readiness = ready.value()[index];
    if (wakes(readiness)) {
      if (auto drained = drain_available(*watched[index]); !drained.has_value()) {
        return drained;
      }
    }
  }
  return core::Ok{};
}

core::Result<core::Ok, ControllerError> Run::drain_available(Tracked& worker) {
  for (int reads = 0; reads < kMaxReadsPerTick; ++reads) {
    if (auto drained = drain(worker); !drained.has_value()) {
      return drained;
    }
    if (!worker.channel_open) {
      break;
    }
    // More already waiting? Only a look: a wait here is the tick's, and the
    // tick has taken its.
    auto more = syscalls_->poll_readable({worker.records.read_fd}, 0);
    if (!more.has_value()) {
      return error_at("record channel", describe(more.error()));
    }
    if (!more.value().front().readable) {
      break;
    }
  }
  return core::Ok{};
}

core::Result<core::Ok, ControllerError> Run::drain(Tracked& worker) {
  auto drained = worker.source->drain();
  if (!drained.has_value()) {
    const std::optional<ipc::FrameError> frame = drained.error().frame;
    if (!frame.has_value()) {
      return error_at("record channel", describe(drained.error()));
    }
    // A frame fault is the worker's, not the controller's: recorded as
    // evidence, and the channel is closed, because nothing after a bad frame
    // is a stream this controller will interpret.
    if (frame->fault == ipc::FrameFault::kTruncated) {
      worker.evidence.channel_truncated = true;
    } else {
      worker.evidence.channel_corrupt = true;
    }
    worker.notes.push_back("record channel: " + describe(*frame));
    close_channel(worker);
    return core::Ok{};
  }
  for (const ipc::Record& record : drained.value().records) {
    if (const auto refusal = ingest(worker, record); refusal.has_value()) {
      // The frame was whole and its checksum right, so these are the bytes the
      // worker sent: and they are not something a worker that works sends.
      // Nothing it says after this is evidence of the machine.
      worker.evidence.channel_corrupt = true;
      worker.notes.push_back("record channel: " + *refusal);
      close_channel(worker);
      return core::Ok{};
    }
  }
  if (drained.value().end_of_stream) {
    close_channel(worker);
  }
  return core::Ok{};
}

std::optional<std::string> Run::ingest(Tracked& worker, const ipc::Record& record) {
  if (record.type == verification::kErrorRecordFrameType) {
    return take_error_record(worker, record);
  }
  if (record.type == worker::kTallyFrameType) {
    return take_tally(worker, record);
  }
  if (record.type == static_cast<std::uint16_t>(ipc::RecordType::kFinished)) {
    worker.evidence.finished_seen = true;
  }
  worker.notes.push_back(std::string{record_word(record.type)} + ": " + text_of(record.payload));
  return std::nullopt;
}

std::optional<std::string> Run::take_error_record(Tracked& worker, const ipc::Record& record) {
  if (!worker.plan.args.has_value()) {
    return "an error record from a worker that runs no workload";
  }
  auto decoded = verification::decode(record.payload);
  if (!decoded.has_value()) {
    return "an error record that does not decode (" +
           std::string{verification::describe(decoded.error().fault)} +
           "): " + decoded.error().detail;
  }
  const verification::ErrorRecord& found = decoded.value();
  // A worker reports on the one workload it was told to run. A record about
  // any other is garbage that happened to be well formed, and a finding made
  // of it would be a verdict on hardware from nothing.
  // Read through a visit and not verification::provenance_of: that one ends in
  // an unchecked get_if, which -Wnull-dereference refuses at -O3 (journal 11).
  const std::optional<std::string> reported =
      std::visit([](const auto& each) { return each.provenance.workload; }, found);
  if (reported.has_value() && *reported != worker.plan.args->workload) {
    return "an error record about workload '" + *reported + "' from a worker that runs '" +
           worker.plan.args->workload + "'";
  }
  if (verification::is_wrong_result(found)) {
    ++worker.wrong_results;
  } else {
    ++worker.not_performed;
  }
  if (worker.findings.size() < kMaxFindingsKept) {
    FindingReport kept;
    kept.record = found;
    worker.findings.push_back(std::move(kept));
  } else {
    ++worker.findings_dropped;
  }
  return std::nullopt;
}

std::optional<std::string> Run::take_tally(Tracked& worker, const ipc::Record& record) {
  auto decoded = worker::decode_tally(record.payload);
  if (!decoded.has_value()) {
    return decoded.error();
  }
  // A tally is cumulative: one that is smaller than the last is not a
  // worker's running total.
  const verification::Tally& now = decoded.value();
  if (worker.tally_seen && (now.units_computed < worker.tally.units_computed ||
                            now.oracle_blocks < worker.tally.oracle_blocks ||
                            now.integrity_checked < worker.tally.integrity_checked)) {
    return "a tally that went backwards";
  }
  worker.tally = now;
  worker.tally_seen = true;
  return std::nullopt;
}

void Run::close_channel(Tracked& worker) {
  (void)syscalls_->close(worker.records.read_fd);
  worker.records.read_fd = -1;
  worker.channel_open = false;
}

core::Result<core::Ok, ControllerError> Run::reap(std::int64_t elapsed_ns) {
  while (live_ > 0) {
    auto death = pool_.reap_next_nonblocking();
    if (!death.has_value()) {
      return error_at("reap", describe(death.error()));
    }
    const std::optional<platform::WorkerDeath> dead = death.value();
    if (!dead.has_value()) {
      return core::Ok{};
    }
    Tracked& worker = workers_[dead->slot];
    worker.evidence.status = dead->status;
    worker.reaped = true;
    if (sampling_.enabled) {
      sampling_.reaps.push_back(Reap{dead->slot, clock_running_ns_});
    }
    --live_;
    kill_group(worker);
    (void)syscalls_->close(worker.control.write_fd);
    worker.control.write_fd = -1;
    reporter_->event(elapsed_ns, "worker " + std::to_string(worker.slot) + " (pid " +
                                     std::to_string(worker.pid) +
                                     "): " + worker.evidence.status.describe());
  }
  return core::Ok{};
}

void Run::kill_group(Tracked& worker) {
  // The worker led a process group of its own (WorkerLauncher::become_worker),
  // and anything it forked is still in it: a grandchild with no death signal,
  // since the kernel clears that across fork, holding a core and the record
  // pipe for as long as nobody kills it. Now that the worker is gone, nothing
  // in its group has a reason to run. ESRCH -- an empty group -- is the usual
  // answer and says nothing; a kill that found someone is recorded, because a
  // worker that left processes behind is part of what it did.
  //
  // Straight after the reap, for the pid's sake: the kernel does not reuse a
  // pid that is still some group's id, so while anything of the worker's is
  // left the number names that group and nothing else. Once nothing is left,
  // reuse needs the pid allocator to wrap all the way round in between.
  auto killed = syscalls_->send_signal(-worker.pid, SIGKILL);
  if (killed.has_value()) {
    worker.notes.emplace_back("process group: killed what the worker left running");
  } else if (killed.error().number != ESRCH) {
    worker.notes.push_back("process group: " + describe(killed.error()));
  }
}

core::Result<core::Ok, ControllerError> Run::drain_after_death() {
  // Every worker is reaped, so every record pipe's writer is gone and end of
  // file is due -- unless something the worker started still holds the
  // write end. That is bounded twice: a channel gets one stop grace of wall
  // time in all, and at most kMaxDrainsAfterDeath drains within it. A count
  // alone let a writer that trickled one byte just inside each grace hold the
  // run for 1024 graces, seventeen minutes at the default; the time alone
  // would let a writer that floods keep the loop busy for the whole grace. A
  // channel that has not ended by either bound is recorded as truncated.
  for (Tracked& worker : workers_) {
    if (!worker.channel_open) {
      continue;
    }
    auto opened = elapsed_now();
    if (!opened.has_value()) {
      return opened.error();
    }
    const std::int64_t give_up_at = opened.value() + options_->grace.stop_ns;
    std::int64_t now_ns = opened.value();
    for (int attempt = 0;
         worker.channel_open && attempt < kMaxDrainsAfterDeath && now_ns < give_up_at; ++attempt) {
      const std::int64_t remaining_ns = give_up_at - now_ns;
      const auto timeout_ms = static_cast<int>((remaining_ns + kNanosecondsPerMillisecond - 1) /
                                               kNanosecondsPerMillisecond);
      auto ready = syscalls_->poll_readable({worker.records.read_fd}, timeout_ms);
      if (!ready.has_value()) {
        return error_at("record channel", describe(ready.error()));
      }
      const platform::Readiness& readiness = ready.value().front();
      if (!wakes(readiness)) {
        break;  // Nothing came, and the writer is not gone: somebody else holds it.
      }
      if (auto drained = drain(worker); !drained.has_value()) {
        return drained;
      }
      auto now = elapsed_now();
      if (!now.has_value()) {
        return now.error();
      }
      now_ns = now.value();
    }
    if (worker.channel_open) {
      worker.evidence.channel_truncated = true;
      worker.notes.emplace_back("record channel: held open after the worker died");
      close_channel(worker);
    }
  }
  return core::Ok{};
}

void Run::telemetry_fault(const std::string& text, std::int64_t now_ns) {
  if (!sampling_.fault.has_value()) {
    sampling_.fault = text;
  }
  reporter_->event(now_ns - started_.running_ns, "telemetry: " + text);
}

void Run::begin_telemetry() {
  if (options_->sampler == nullptr) {
    return;
  }
  sampling_.enabled = true;
  sampling_.interval_ns = options_->sample_interval_ns;
  sampling_live_ = true;
  // Before the first fork: the baseline every counter's delta is taken from.
  take_round(started_.running_ns, true);
  mark_phase("run", started_.running_ns);
}

void Run::take_round(std::int64_t now_ns, bool force) {
  if (!sampling_live_) {
    return;
  }
  const bool taken = sampling_.rounds > 0;
  // The timeline refuses a round that is not later than the last: skipped, as
  // a round that is not due is, rather than offered and refused.
  if (taken && now_ns <= sampling_.last_round_ns) {
    return;
  }
  if (taken && !force && now_ns - sampling_.last_round_ns < options_->sample_interval_ns) {
    return;
  }
  const auto answer = options_->sampler->round(now_ns);
  // The observer's cost (F17). A clock that cannot be read leaves the cost a
  // lower bound, which is the only honest thing to say about it.
  if (const auto after = clock_.now(); after.has_value()) {
    sampling_.cost_ns += std::max<std::int64_t>(0, after.value().running_ns - now_ns);
  }
  if (!answer.has_value()) {
    telemetry_fault("sampling stopped: " + describe_refusal(answer.error()), now_ns);
    sampling_live_ = false;
    return;
  }
  if (!taken) {
    sampling_.first_round_ns = now_ns;
  }
  sampling_.last_round_ns = now_ns;
  ++sampling_.rounds;
  sampling_.points += answer.value();
}

void Run::mark_phase(std::string_view label, std::int64_t now_ns) {
  if (!sampling_.enabled || options_->timeline == nullptr) {
    return;
  }
  const auto marked = options_->timeline->mark_phase({now_ns, std::string{label}});
  if (!marked.has_value()) {
    telemetry_fault("phase \"" + std::string{label} +
                        "\" refused: " + std::string{telemetry::describe(marked.error().fault)},
                    now_ns);
  }
}

void Run::attribute_oom_kills() {
  // Every worker is reaped by now, so the deaths and the count are complete:
  // the window judge_oom_kills needs. The count is read again only when a
  // death needs it.
  if (options_->oom_record == nullptr) {
    return;
  }
  std::vector<Tracked*> candidates;
  for (Tracked& worker : workers_) {
    if (oom_candidate(worker.evidence)) {
      candidates.push_back(&worker);
    }
  }
  if (candidates.empty()) {
    return;
  }
  const auto after = options_->oom_record->kills();
  const OomFinding finding =
      judge_oom_kills(oom_before_, after, candidates.size(), options_->oom_record->scope());
  for (Tracked* worker : candidates) {
    worker->evidence.oom_kill_recorded = finding.attributed;
    worker->evidence.oom_inconclusive = !finding.attributed && !finding.ruled_out;
    worker->notes.push_back(finding.note);
  }
}

core::Result<RunReport, ControllerError> Run::finish() {
  if (auto drained = drain_after_death(); !drained.has_value()) {
    return drained.error();
  }
  attribute_oom_kills();
  auto elapsed_ns = elapsed_now();
  if (!elapsed_ns.has_value()) {
    return elapsed_ns.error();
  }
  RunReport report;
  report.elapsed_ns = elapsed_ns.value();
  for (Tracked& worker : workers_) {
    WorkerReport entry;
    entry.slot = worker.slot;
    entry.pid = worker.pid;
    entry.fate = classify(worker.evidence);
    entry.termination = worker.evidence.status.describe();
    entry.stop = std::string{describe(worker.step)};
    entry.progress = worker.progress;
    entry.records = std::move(worker.notes);
    entry.tally = worker.tally;
    entry.tally_seen = worker.tally_seen;
    entry.work = worker.plan.work;
    entry.findings = std::move(worker.findings);
    entry.wrong_results = worker.wrong_results;
    entry.not_performed = worker.not_performed;
    entry.findings_dropped = worker.findings_dropped;
    report.workers.push_back(std::move(entry));
  }
  // After the last reap: every counter now holds what the workers did, and
  // what they left behind when they died.
  take_round(clock_running_ns_, true);
  report.sampling = sampling_;
  auto released = region_->release();
  if (!released.has_value()) {
    return error_at("heartbeat", describe(released.error()));
  }
  return report;
}

void Run::teardown() {
  // The silent release, for every way out. Live workers are killed and reaped
  // so a failed run leaves none behind; descriptors and the region follow.
  // Gated on the spawn alone: with none left live, every worker is reaped and
  // its pid cleared from the pool, so the kills below find nobody to send to
  // and the wait is never entered. The child of a fork never gets here with
  // spawned_ set, and must not: its siblings are not its to kill.
  if (spawned_) {
    // Each unreaped worker's group first, while its pid still pins the group's
    // number (see kill_group), then the workers themselves, for any that died
    // or failed before leading a group. A worker whose pid was never recorded
    // -- the spawn failed after the fork -- is skipped: -0 is not its group but
    // this process's own, and the kill would end the controller.
    for (const Tracked& worker : workers_) {
      if (!worker.reaped && worker.pid > 0) {
        (void)syscalls_->send_signal(-worker.pid, SIGKILL);
      }
    }
    (void)pool_.signal_all(SIGKILL);
    while (live_ > 0) {
      auto reaped = pool_.wait_for_next();
      if (!reaped.has_value()) {
        break;
      }
      --live_;
    }
  }
  for (Tracked& worker : workers_) {
    for (int* fd : {&worker.records.read_fd, &worker.records.write_fd, &worker.control.read_fd,
                    &worker.control.write_fd}) {
      if (*fd >= 0) {
        (void)syscalls_->close(*fd);
        *fd = -1;
      }
    }
    worker.channel_open = false;
  }
  region_.reset();
}

core::Result<RunReport, ControllerError> Run::execute() {
  if (auto prepared = prepare(); !prepared.has_value()) {
    return prepared.error();
  }
  RunPlan plan;
  plan.mode = config_->run.mode;
  plan.duration = config_->run.duration;
  plan.workers = workers_.size();
  plan.executable = options_->executable;
  plan.limits = limit_statuses(*config_, options_->budget, options_->budget_unread);
  if (options_->topology != nullptr) {
    plan.topology = topology::describe(*options_->topology);
  } else {
    plan.topology = "no snapshot offered";
  }
  for (const Tracked& worker : workers_) {
    plan.workloads.push_back(worker.plan.description);
  }
  plan.environment = environment_;
  // Probed once, before the first fork, by the caller's sampler: the banner
  // states the set that was found, and this run probes nothing.
  plan.capabilities = options_->capabilities;
  reporter_->run_starting(plan);

  auto spawned = spawn();
  if (!spawned.has_value()) {
    return spawned.error();
  }
  const std::optional<int> child_code = spawned.value();
  if (child_code.has_value()) {
    RunReport child;
    child.worker_exit_code = child_code;
    return child;
  }
  while (live_ > 0) {
    if (auto ticked = tick(); !ticked.has_value()) {
      return ticked.error();
    }
  }
  auto report = finish();
  if (report.has_value()) {
    reporter_->run_finished(report.value());
  }
  return report;
}

}  // namespace

core::Result<RunReport, ControllerError> Controller::run(const config::Config& config,
                                                         const RunOptions& options) {
  Run run(*syscalls_, *reporter_, config, options);
  return run.execute();
}

}  // namespace loadforge::controller
