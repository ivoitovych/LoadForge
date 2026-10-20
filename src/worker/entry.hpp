// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_ENTRY_HPP
#define LOADFORGE_WORKER_ENTRY_HPP

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/syscalls.hpp"
#include "worker/scheduler.hpp"

namespace loadforge::worker {

/// The internal switch (docs/PLAN.md §4.6): the controller re-executes this
/// binary with it, and nothing else does. It is not in --help because it is
/// not a command a person runs; it is not a test hook because it does exactly
/// what a worker does in production and nothing a test could ask for.
inline constexpr std::string_view kWorkerSwitch = "--worker";

/// How a worker process ends. Exit codes, so the same rules as
/// platform::WorkerStartupFailure: distinct from 0-2, from the shell's 126
/// and 127, from the launcher's 120-123, and below 256.
///
///   kCompleted       ran to the end of its duration, or was asked to stop.
///   kSetupFailed     could not attach, could not report, could not be placed:
///                    the worker's own failure before or around its work.
///   kControllerLost  stopped because the controller hung up, went stale or
///                    could not be read -- a record pipe with no reader left
///                    (EPIPE) included. Not the worker's fault, and not a
///                    completion.
///   kBadArguments    the internal switch was used wrongly: a controller bug.
enum class WorkerExit : std::uint8_t {
  kCompleted = 0,
  kSetupFailed = 119,
  kControllerLost = 124,
  kBadArguments = 125,
};

/// Everything the controller tells a worker. The descriptors are the ones
/// WorkerLauncher::become_worker kept across exec; the rest is pacing.
struct WorkerArgs {
  std::size_t slot = 0;
  std::size_t workers = 0;
  int heartbeat_fd = -1;
  int record_fd = -1;
  int control_fd = -1;
  pid_t supervisor_pid = 0;
  Pacing pacing;
  std::vector<std::uint32_t> cpus;  ///< Empty: wherever the cpuset allows.

  [[nodiscard]] friend bool operator==(const WorkerArgs&, const WorkerArgs&) = default;
};

/// Parses what follows the switch. Every key is required once, except
/// --cpus; every value has a stated domain; anything else is refused with a
/// message naming the key. The controller writes these, so a refusal is a
/// bug report, and the message says exactly which argument.
[[nodiscard]] core::Result<WorkerArgs, std::string> parse_args(
    std::span<const std::string_view> args);

/// The inverse: the argv a controller execs a worker with. parse_args of this
/// yields `args` again, and a test says so, which is what keeps the two from
/// drifting (the config module's round-trip rule).
[[nodiscard]] std::vector<std::string> argv_for(std::string_view executable,
                                                const WorkerArgs& args);

/// The worker process, from after the switch to its exit code. Diagnostics go
/// to `err`, which is the worker's stderr: the controller does not read it,
/// so it is for a person watching, and the record pipe carries what the
/// controller needs.
[[nodiscard]] int run(platform::Syscalls& syscalls, std::span<const std::string_view> args,
                      std::ostream& err);

}  // namespace loadforge::worker

#endif
