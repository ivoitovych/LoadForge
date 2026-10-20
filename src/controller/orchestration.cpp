// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/orchestration.hpp"

#include <sys/types.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "controller/safety.hpp"
#include "controller/supervision.hpp"
#include "core/result.hpp"
#include "platform/clock.hpp"
#include "platform/exit_status.hpp"
#include "platform/process.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "platform/worker_pool.hpp"
#include "worker/entry.hpp"
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

std::string describe(const ControllerError& error) { return error.stage + ": " + error.detail; }

namespace {

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
constexpr std::uint32_t kControllerPhase = 1;
/// How many drains a channel is given after its worker is dead before the
/// controller concludes that something else holds the write end open.
constexpr int kMaxDrainsAfterDeath = 1024;

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
  std::vector<std::string> notes;
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
                                                              const platform::TimePoint& now,
                                                              std::int64_t elapsed_ns);
  [[nodiscard]] core::Result<core::Ok, ControllerError> send(Tracked& worker, int signal,
                                                             std::int64_t now_ns, bool stale);
  [[nodiscard]] core::Result<core::Ok, ControllerError> poll_and_drain(int timeout_ms);
  [[nodiscard]] core::Result<core::Ok, ControllerError> drain(Tracked& worker);
  [[nodiscard]] core::Result<core::Ok, ControllerError> reap(std::int64_t elapsed_ns);
  [[nodiscard]] core::Result<core::Ok, ControllerError> drain_after_death();
  [[nodiscard]] core::Result<RunReport, ControllerError> finish();
  void close_channel(Tracked& worker);
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
  std::size_t live_ = 0;
  bool stop_requested_ = false;
  bool spawned_ = false;
};

core::Result<core::Ok, ControllerError> Run::prepare() {
  if (auto memory = check_memory(*config_, options_->budget); !memory.has_value()) {
    return error_at("memory", memory.error());
  }
  for (const config::Workload& workload : config_->workloads) {
    if (workload.enabled) {
      workers_.push_back(Tracked{});
      workers_.back().slot = workers_.size() - 1;
    }
  }
  if (workers_.empty()) {
    return error_at("config", "no workload is enabled");
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
  beat(started_);

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
    argvs.push_back(worker::argv_for(options_->executable, args));
  }

  auto spawned = pool_.spawn(argvs);
  if (!spawned.has_value()) {
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
    worker.progress_changed_ns = started_.running_ns;
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
  beat(now.value());
  if (!stop_requested_ && elapsed_ns >= duration_ns_) {
    if (auto requested = request_stop(now.value().running_ns); !requested.has_value()) {
      return requested;
    }
    reporter_->event(elapsed_ns, "deadline reached: stop requested");
  }
  for (Tracked& worker : workers_) {
    if (!worker.reaped) {
      if (auto watched = watch(worker, now.value(), elapsed_ns); !watched.has_value()) {
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

core::Result<core::Ok, ControllerError> Run::watch(Tracked& worker, const platform::TimePoint& now,
                                                   std::int64_t elapsed_ns) {
  // The worker's beat: stale by timestamp is the hang; fresh but with no
  // progress for the same allowance is the spin. Both are the same decision,
  // and a slot that cannot be read is a worker that died inside publish(),
  // treated as stale because it will not beat again.
  bool stale = false;
  auto observed = region_->observe(ipc::worker_slot(worker.slot));
  if (!observed.has_value()) {
    stale = true;
  } else {
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
      stale = elapsed_ns > options_->stale_after_ns;
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
  // ESRCH is a worker that died since the last reap: not an error, and the
  // reap that follows will say how.
  auto sent = syscalls_->send_signal(worker.pid, signal);
  if (!sent.has_value() && sent.error().number != ESRCH) {
    return error_at("signal", describe(sent.error()));
  }
  worker.step = step_after(signal);
  worker.step_since_ns = now_ns;
  worker.evidence.signal_sent = signal;
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
      if (auto drained = drain(*watched[index]); !drained.has_value()) {
        return drained;
      }
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
    if (record.type == static_cast<std::uint16_t>(ipc::RecordType::kFinished)) {
      worker.evidence.finished_seen = true;
    }
    worker.notes.push_back(std::string{record_word(record.type)} + ": " + text_of(record.payload));
  }
  if (drained.value().end_of_stream) {
    close_channel(worker);
  }
  return core::Ok{};
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
    --live_;
    (void)syscalls_->close(worker.control.write_fd);
    worker.control.write_fd = -1;
    reporter_->event(elapsed_ns, "worker " + std::to_string(worker.slot) + " (pid " +
                                     std::to_string(worker.pid) +
                                     "): " + worker.evidence.status.describe());
  }
  return core::Ok{};
}

core::Result<core::Ok, ControllerError> Run::drain_after_death() {
  // Every worker is reaped, so every record pipe's writer is gone and end of
  // file is due -- unless something the worker started still holds the
  // write end. That is bounded: a channel that stays open without delivering
  // its end is recorded as truncated and closed.
  const auto grace_ms = static_cast<int>(options_->grace.stop_ns / kNanosecondsPerMillisecond);
  for (Tracked& worker : workers_) {
    for (int attempt = 0; worker.channel_open && attempt < kMaxDrainsAfterDeath; ++attempt) {
      auto ready = syscalls_->poll_readable({worker.records.read_fd}, grace_ms);
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
    }
    if (worker.channel_open) {
      worker.evidence.channel_truncated = true;
      worker.notes.emplace_back("record channel: held open after the worker died");
      close_channel(worker);
    }
  }
  return core::Ok{};
}

core::Result<RunReport, ControllerError> Run::finish() {
  if (auto drained = drain_after_death(); !drained.has_value()) {
    return drained.error();
  }
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
    report.workers.push_back(std::move(entry));
  }
  auto released = region_->release();
  if (!released.has_value()) {
    return error_at("heartbeat", describe(released.error()));
  }
  return report;
}

void Run::teardown() {
  // The silent release, for every way out. Live workers are killed and reaped
  // so a failed run leaves none behind; descriptors and the region follow.
  if (spawned_ && live_ > 0) {
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
  plan.limits = limit_statuses(*config_, options_->budget);
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
