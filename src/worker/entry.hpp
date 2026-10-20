// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_ENTRY_HPP
#define LOADFORGE_WORKER_ENTRY_HPP

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/record.hpp"
#include "worker/scheduler.hpp"
#include "worker/services.hpp"

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

/// The internal switch of a re-run (docs/verification.md §7): the controller
/// executes this binary with it, once per rung of the isolation ladder, to run
/// ONE unit again on a chosen CPU with a chosen thread count and say, by its
/// exit code, what came of it. Not in --help, for the reason --worker is not.
inline constexpr std::string_view kRerunSwitch = "--rerun";

/// How a re-run ends. Distinct from the worker's codes (0, 119, 124, 125) and
/// from the launcher's 120-123, so a controller can tell a re-run that was
/// made from one that was not even started.
///
///   kClean        the unit was computed, checked by the oracle and bracketed,
///                 and nothing was wrong.
///   kReproduced   the unit came out wrong again: a wrong-result record.
///   kNotPerformed the re-run could not be made, or could not check: no
///                 workload or oracle of that description, the placement was
///                 refused, the oracle answered NotChecked.
///   kBadArguments the switch was used wrongly: a controller bug.
enum class RerunExit : std::uint8_t {
  kClean = 0,
  kReproduced = 20,
  kNotPerformed = 21,
  kBadArguments = 22,
};

/// What a worker runs when it runs a real workload. Absent: the null workload
/// of M1, which does nothing but everything else a worker does.
///
///   workload, seed, footprint_bytes, decomposition, isa   the WorkloadConfig
///        the controller chose, once, for every worker and every re-run.
///   threads        the executor's thread count, at least 1.
///   duty           the duty cycle recorded in every record.
///   line_bytes     the topology's cache-line size (record.hpp, ExactBitCorruption).
///   physical_core, numa_node   where --cpus puts the worker, from the
///        topology the controller took; absent when the worker is not placed
///        on one CPU.
struct WorkloadArgs {
  std::string workload;
  std::uint64_t seed = 0;
  std::uint64_t footprint_bytes = 0;
  std::uint32_t threads = 1;
  std::uint32_t decomposition = 0;
  simd::IsaPath isa = simd::IsaPath::kScalar;
  verification::DutyCycle duty;
  std::uint32_t line_bytes = 64;
  std::optional<std::uint32_t> physical_core;
  std::optional<std::uint32_t> numa_node;
};

// Defined in entry.cpp, once: a defaulted comparison in a header reads as
// uncovered from every translation unit but one (journal §1.22).
[[nodiscard]] bool operator==(const WorkloadArgs& lhs, const WorkloadArgs& rhs) noexcept;

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
  std::optional<WorkloadArgs> workload;
};

[[nodiscard]] bool operator==(const WorkerArgs& lhs, const WorkerArgs& rhs) noexcept;

/// One re-run's arguments: the unit (workload, its version, the index, and the
/// configuration that produced it) and where to run it.
struct RerunArgs {
  WorkloadArgs unit;
  std::uint32_t workload_version = 0;
  std::uint64_t iteration = 0;
  std::uint32_t cpu = 0;
};

[[nodiscard]] bool operator==(const RerunArgs& lhs, const RerunArgs& rhs) noexcept;

/// Parses what follows the switch. Every key is required once, except
/// --cpus; every value has a stated domain; anything else is refused with a
/// message naming the key. The controller writes these, so a refusal is a
/// bug report, and the message says exactly which argument.
[[nodiscard]] core::Result<WorkerArgs, std::string> parse_args(
    std::span<const std::string_view> args);

/// Parses what follows --rerun: a unit, the version of the workload that made
/// the finding, the unit's index and the CPU to run it on. Every key is
/// required once; anything else is refused with a message naming the key.
[[nodiscard]] core::Result<RerunArgs, std::string> parse_rerun_args(
    std::span<const std::string_view> args);

/// The inverse of parse_rerun_args, as argv_for is of parse_args.
[[nodiscard]] std::vector<std::string> rerun_argv_for(std::string_view executable,
                                                      const RerunArgs& args);

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

/// The same with the services named: what a test that wants a worker built
/// from doubles passes, and what the line above passes ProductionServices for.
[[nodiscard]] int run(platform::Syscalls& syscalls, std::span<const std::string_view> args,
                      std::ostream& err, const Services& services);

/// The re-run process, from after --rerun to its exit code (RerunExit): one
/// unit, on one CPU, with every check on, and what became of it.
[[nodiscard]] int run_rerun(platform::Syscalls& syscalls, std::span<const std::string_view> args,
                            std::ostream& err, const Services& services);

}  // namespace loadforge::worker

#endif
