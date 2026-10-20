// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/supervision.hpp"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "core/result.hpp"
#include "platform/exit_status.hpp"

namespace loadforge::controller {

std::string_view describe(WorkerFate fate) noexcept {
  switch (fate) {
    case WorkerFate::kCompleted:
      return "completed";
    case WorkerFate::kWorkerError:
      return "worker error";
    case WorkerFate::kStartupFailed:
      return "startup failed";
    case WorkerFate::kControllerLostReported:
      return "worker reported the controller lost";
    case WorkerFate::kFault:
      return "fault";
    case WorkerFate::kKilled:
      return "killed";
    case WorkerFate::kOomKilled:
      return "killed by the OOM killer";
    case WorkerFate::kStoppedByController:
      return "stopped by the controller";
    case WorkerFate::kTimedOut:
      return "timed out";
    case WorkerFate::kChannelCorrupt:
      return "record channel corrupt";
    case WorkerFate::kTruncatedEvidence:
      return "evidence truncated";
    case WorkerFate::kUnrecognised:
      return "unrecognised termination";
  }
  return "unrecognised fate";
}

bool is_fault_signal(int signal) noexcept {
  return signal == SIGSEGV || signal == SIGBUS || signal == SIGABRT || signal == SIGILL ||
         signal == SIGFPE || signal == SIGTRAP;
}

namespace {

// The exit codes a worker can use before it did any work: the launcher's
// (platform::WorkerStartupFailure) and the worker's own refusal of its
// arguments (worker::WorkerExit::kBadArguments). Named here as numbers so this
// module depends on neither header; the worker's tests pin the values.
constexpr int kFirstLauncherCode = 120;
constexpr int kLastLauncherCode = 123;
constexpr int kControllerLostCode = 124;
constexpr int kBadArgumentsCode = 125;

WorkerFate classify_exit(const Evidence& evidence) noexcept {
  const int code = evidence.status.code();
  if (code == 0) {
    // A clean exit is clean only with the finished record in hand: a worker
    // that exited 0 and said nothing has left no evidence that it finished.
    return evidence.finished_seen && !evidence.channel_truncated ? WorkerFate::kCompleted
                                                                 : WorkerFate::kTruncatedEvidence;
  }
  if ((code >= kFirstLauncherCode && code <= kLastLauncherCode) || code == kBadArgumentsCode) {
    return WorkerFate::kStartupFailed;
  }
  if (code == kControllerLostCode) {
    return WorkerFate::kControllerLostReported;
  }
  return WorkerFate::kWorkerError;
}

WorkerFate classify_signal(const Evidence& evidence) noexcept {
  const int signal = evidence.status.signal();
  if (evidence.signal_sent.has_value() && *evidence.signal_sent == signal) {
    return evidence.stale_when_signalled ? WorkerFate::kTimedOut : WorkerFate::kStoppedByController;
  }
  if (is_fault_signal(signal)) {
    return WorkerFate::kFault;
  }
  // The OOM killer sends SIGKILL and nothing else, so the kernel's record
  // can explain no other signal: a SIGTERM from outside stays a kill however
  // the count moved.
  if (signal == SIGKILL && evidence.oom_kill_recorded) {
    return WorkerFate::kOomKilled;
  }
  return WorkerFate::kKilled;
}

constexpr std::string_view kNotAttributed = "not attributed to the OOM killer: ";

}  // namespace

WorkerFate classify(const Evidence& evidence) noexcept {
  // Corruption first, whatever else happened: a worker whose channel carried
  // a frame that did not verify has produced findings this tool will not
  // interpret, and no exit code makes them trustworthy.
  if (evidence.channel_corrupt) {
    return WorkerFate::kChannelCorrupt;
  }
  // Two comparisons rather than a switch: a switch over the enumeration
  // carries a range check for values outside it that nothing can produce,
  // which would be a branch no test could take.
  const platform::Termination termination = evidence.status.termination();
  if (termination == platform::Termination::kExited) {
    return classify_exit(evidence);
  }
  if (termination == platform::Termination::kSignalled) {
    return classify_signal(evidence);
  }
  // Stopped, continued, or a word the decoder could not classify: none is a
  // termination, and a classifier that cannot classify says so (F20).
  return WorkerFate::kUnrecognised;
}

bool oom_candidate(const Evidence& evidence) noexcept {
  if (evidence.channel_corrupt ||
      evidence.status.termination() != platform::Termination::kSignalled) {
    return false;
  }
  return evidence.status.signal() == SIGKILL && evidence.signal_sent != SIGKILL;
}

OomFinding judge_oom_kills(const core::Result<std::uint64_t, std::string>& before,
                           const core::Result<std::uint64_t, std::string>& after,
                           std::size_t candidates, std::string_view scope) {
  // One allocating step per statement throughout: a message built as one
  // expression inside the aggregate would leave cleanup edges no test can
  // take (journal §1.13).
  std::string note{kNotAttributed};
  if (!before.has_value()) {
    note += "the OOM-kill count could not be read before the run: ";
    note += before.error();
    return OomFinding{false, std::move(note)};
  }
  if (!after.has_value()) {
    note += "the OOM-kill count could not be read after the run: ";
    note += after.error();
    return OomFinding{false, std::move(note)};
  }
  const std::uint64_t first = before.value();
  const std::uint64_t last = after.value();
  note += scope;
  if (last < first) {
    note += " went from " + std::to_string(first) + " to " + std::to_string(last);
    note += ", so it is not the count that was read before the run";
    return OomFinding{false, std::move(note)};
  }
  const std::uint64_t counted = last - first;
  if (counted == 0) {
    note += " counted no OOM kill during the run";
    return OomFinding{false, std::move(note)};
  }
  if (counted < candidates) {
    note += " counted " + std::to_string(counted) + " OOM kill(s) during the run for ";
    note += std::to_string(candidates) + " workers killed by a SIGKILL the controller did not ";
    note += "send, and a count does not say which";
    return OomFinding{false, std::move(note)};
  }
  std::string found = "OOM kill: ";
  found += scope;
  found += " counted " + std::to_string(counted) + " during the run, one or more for each of the ";
  found +=
      std::to_string(candidates) + " worker(s) killed by a SIGKILL the controller did not send";
  return OomFinding{true, std::move(found)};
}

}  // namespace loadforge::controller
