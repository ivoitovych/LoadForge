// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/rerun.hpp"

#include <sys/types.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "controller/report.hpp"
#include "platform/exit_status.hpp"
#include "platform/process.hpp"
#include "platform/syscalls.hpp"
#include "platform/worker_pool.hpp"
#include "verification/isolation.hpp"
#include "verification/record.hpp"
#include "worker/entry.hpp"

namespace loadforge::controller {
namespace {

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
/// How long one look at a re-run waits.
constexpr int kPollMs = 100;

/// The provenance of the wrong result, whichever kind it is.
const verification::Provenance& where_of(const verification::WrongResult& finding) {
  return std::visit(
      [](const auto& held) -> const verification::Provenance& { return held.provenance; }, finding);
}

/// What a re-run's end says. A clean exit is clean and the reproduction code
/// is a reproduction; everything else -- the other codes, and every death by a
/// signal -- is a re-run that did not answer.
verification::RerunOutcome outcome_of(const platform::ExitStatus& status) {
  if (status.termination() != platform::Termination::kExited) {
    return verification::RerunOutcome::kNotPerformed;
  }
  if (status.code() == static_cast<int>(worker::RerunExit::kClean)) {
    return verification::RerunOutcome::kClean;
  }
  if (status.code() == static_cast<int>(worker::RerunExit::kReproduced)) {
    return verification::RerunOutcome::kReproduced;
  }
  return verification::RerunOutcome::kNotPerformed;
}

}  // namespace

RealRerunner::RealRerunner(platform::Syscalls& syscalls, std::string executable,
                           RerunOptions options)
    : syscalls_(&syscalls), executable_(std::move(executable)), options_(options) {}

void RealRerunner::use(const WorkerWork& work) { work_ = work; }

std::optional<int> RealRerunner::child_exit_code() const noexcept { return child_exit_; }

verification::RerunOutcome RealRerunner::rerun(const verification::WrongResult& finding,
                                               const verification::RerunRequest& request) {
  using verification::RerunOutcome;
  if (child_exit_.has_value() || !work_.has_value() || request.threads == 0) {
    return RerunOutcome::kNotPerformed;
  }
  const verification::Provenance& where = where_of(finding);
  // The unit is what the provenance names. A field the producer left unset is
  // a fact it did not have, and a re-run of a guess is not a re-run.
  if (!where.workload.has_value() || !where.workload_version.has_value() ||
      !where.iteration.has_value() || !where.seed.has_value() || !where.isa.has_value() ||
      *where.workload != work_->descriptor.name) {
    return RerunOutcome::kNotPerformed;
  }
  worker::RerunArgs args;
  args.unit.workload = *where.workload;
  args.unit.seed = *where.seed;
  args.unit.footprint_bytes = work_->footprint_bytes;
  args.unit.threads = request.threads;
  args.unit.decomposition = work_->decomposition;
  args.unit.isa = *where.isa;
  args.unit.line_bytes = work_->line_bytes;
  args.workload_version = *where.workload_version;
  args.iteration = *where.iteration;
  args.cpu = request.cpu;
  const std::vector<std::string> argv = worker::rerun_argv_for(executable_, args);

  // The child's end of this pipe is the only thing that tells a re-run that
  // has ended from one that is still going without a blocking wait: it is
  // closed when the child exits, by whatever means.
  auto pipe = syscalls_->create_pipe();
  if (!pipe.has_value()) {
    return RerunOutcome::kNotPerformed;
  }
  platform::WorkerLauncher launcher(*syscalls_, executable_);
  platform::WorkerPool pool(*syscalls_, launcher);
  const pid_t self = syscalls_->process_id();
  auto spawned = pool.spawn({argv});
  if (!spawned.has_value()) {
    (void)syscalls_->close(pipe.value().read_fd);
    (void)syscalls_->close(pipe.value().write_fd);
    return RerunOutcome::kNotPerformed;
  }
  if (spawned.value().is_worker()) {
    child_exit_ = static_cast<int>(launcher.become_worker(self, argv, {pipe.value().write_fd}));
    return RerunOutcome::kNotPerformed;
  }
  const pid_t child = spawned.value().pids().front();
  (void)syscalls_->close(pipe.value().write_fd);

  // The wait is a count of polls, not a reading of a clock: a re-run that has
  // not ended after timeout_ns of polling is ended, and a clock to measure
  // that by would be one more call that can fail.
  const std::int64_t polls = options_.timeout_ns / (kPollMs * kNanosecondsPerMillisecond);
  bool ended = false;
  for (std::int64_t attempt = 0; attempt < polls && !ended; ++attempt) {
    auto ready = syscalls_->poll_readable({pipe.value().read_fd}, kPollMs);
    if (!ready.has_value()) {
      break;
    }
    const platform::Readiness& state = ready.value().front();
    ended = state.hung_up || state.error || state.invalid;
  }
  (void)syscalls_->close(pipe.value().read_fd);
  if (!ended) {
    // Out of time, or the wait itself failed: the child is ended here, and
    // what it would have said is not known.
    (void)syscalls_->send_signal(child, SIGKILL);
  }
  auto death = pool.wait_for_next();
  if (!death.has_value() || !ended) {
    return RerunOutcome::kNotPerformed;
  }
  return outcome_of(death.value().status);
}

}  // namespace loadforge::controller
