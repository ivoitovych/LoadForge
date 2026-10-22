// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_MIXED_SCHEDULE_HPP
#define LOADFORGE_WORKLOADS_MIXED_SCHEDULE_HPP

// A schedule: what the controller tells each worker, and when, so that a run
// is more than one workload held steady (docs/DESIGN-DRAFT.md §17 and §35).
// It is data, and it is made before the run: a pure function of its options,
// so that two runs of one seed are the same run, the schedule can be read
// before it is run, and what the controller does with it (send commands, mark
// phases, journal) has nothing to decide.
//
//   mixed    the flagship (mixed.hpp): workloads rotate across the workers,
//            their datasets change size, at times every worker joins one
//            operation, and the workers move across CPUs -- while the workers
//            go on verifying every unit.
//   cycling  thermal and power cycling (cycling.hpp): the load rises and falls
//            as phases of duty, or the number of workers that work does.
//
// The schedule speaks only the commands of worker/control.hpp; the worker
// carries them out. A schedule never stops a worker: the run's end is the
// controller's deadline.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "worker/control.hpp"

namespace loadforge::mixed {

/// What one worker is told at a step, in order.
struct WorkerCommands {
  std::size_t slot = 0;
  std::vector<worker::Command> commands;
};

/// The commands of `slot` compare equal when their sequences of commands do.
[[nodiscard]] bool operator==(const WorkerCommands& lhs, const WorkerCommands& rhs);

/// One instant of the schedule.
///
///   at_ns     since the run began (the controller's clock for the deadline),
///             ascending over the schedule and not negative.
///   phase     the label the timeline's phase mark and the journal carry; at
///             most journal::kMaxLabelBytes, and different at every step so
///             that a counter's advance can be told per phase.
///   workload  what the machine is doing, in a label of the same size.
///   workers   what each worker named is told; a worker not named is left
///             as it is.
struct Step {
  std::int64_t at_ns = 0;
  std::string phase;
  std::string workload;
  std::vector<WorkerCommands> workers;
};

[[nodiscard]] bool operator==(const Step& lhs, const Step& rhs);

/// What a worker starts as: the workload it is launched with, so that the
/// run does not begin with a rotation away from something arbitrary.
///
/// A worker's seed at launch is the controller's rule (plan.hpp: the run's seed
/// and the worker's slot), the same for every run of a workload; what a
/// schedule chooses is which workload, and how large.
struct Start {
  std::string workload;
  std::uint64_t footprint_bytes = 0;
};

[[nodiscard]] bool operator==(const Start& lhs, const Start& rhs);

struct Schedule {
  std::size_t workers = 0;
  /// One per worker, in slot order, when the schedule chooses what the
  /// workers start as; empty when they start as the configuration says
  /// (cycling runs the workload it is given).
  std::vector<Start> start;
  std::string phase;     ///< The phase the run begins in, before the first step.
  std::string workload;  ///< And what the machine is doing then.
  std::vector<Step> steps;
};

[[nodiscard]] bool operator==(const Schedule& lhs, const Schedule& rhs);

/// What is wrong with a schedule that is to run `workers` workers, or nothing:
/// a different number of workers, a start that is not one per worker, a step
/// that does not come after the one before it or comes before the run, a worker
/// that is not one of the run's, a command the rules of worker/control.hpp
/// refuse, or a label past the journal's limit.
[[nodiscard]] std::optional<std::string> problem(const Schedule& schedule, std::size_t workers);

/// A digest of everything a schedule says, the same on every architecture
/// (CRC-32 over a canonical text of it), so a test can pin a schedule and CI
/// can compare it across x86-64 and ARM64.
[[nodiscard]] std::uint32_t digest(const Schedule& schedule);

/// A label cut to the journal's limit, on a character boundary of ASCII.
[[nodiscard]] std::string bounded(std::string label);

}  // namespace loadforge::mixed

#endif
