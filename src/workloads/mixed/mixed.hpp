// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_MIXED_MIXED_HPP
#define LOADFORGE_WORKLOADS_MIXED_MIXED_HPP

// The flagship: the dynamic mixed-system reliability test of
// docs/DESIGN-DRAFT.md §17. Different workers run different workloads at once;
// after an interval roles rotate, working sets change size, and at other
// points every worker joins one operation; and, if asked, the workers move
// across the CPUs they may use. The run moves through phases that say what it
// is doing. Throughout, each worker verifies every unit it computes, because
// what rotates is the workload a worker's runner is made for, not the checking
// (worker/control.hpp, Switch and Rebind).
//
// WHAT A PHASE IS
// ---------------
//   compute-heavy   every worker runs a workload of arithmetic intensity.
//   memory-heavy    every worker runs one whose time goes to memory.
//   balanced        half the workers run each.
//   mixed           every worker runs a different workload from the whole
//                   roster, as far as the roster goes.
//   all-join        every worker runs the same workload: one large common
//                   operation.
//   move            nothing changes but where the workers run.
//
// The classes are by the usual reading of each algorithm's arithmetic
// intensity (dense linear algebra, FFT, Monte Carlo, particles and ray tracing
// against sort, stencil, sparse, graph and adaptive mesh), written down in
// classify() and nowhere inferred: a hypothesis about what stresses what, and
// a reason to hold hardware validation (T11) to it. A workload the table does
// not know is balanced. "Coherence-heavy" of the design draft has no workload
// to be: it belongs to the primitives (docs/IMPLEMENTATION.md, stream 20), not
// to a workload a worker verifies unit by unit.
//
// DETERMINISM
// -----------
// Everything is a function of the options. The pseudo-random choices are one
// 64-bit mixing function of (seed, rotation, worker, purpose), written out
// here, with no library generator whose output a standard may change, so the
// schedule is the same on every architecture (a test pins its digest).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "verification/record.hpp"
#include "workloads/mixed/schedule.hpp"
#include "workloads/workload.hpp"

namespace loadforge::mixed {

/// A workload a schedule may use and the least footprint it accepts.
struct RosterEntry {
  std::string name;
  std::uint64_t min_footprint_bytes = 0;
};

[[nodiscard]] bool operator==(const RosterEntry& lhs, const RosterEntry& rhs);

/// Every workload the registry names that accepts some footprint up to
/// `ceiling_bytes`, with the least footprint (in doublings from 64 KiB) at
/// which the registry creates it, in the registry's order. A workload that
/// accepts none is left out: a schedule cannot run what cannot be made.
[[nodiscard]] std::vector<RosterEntry> roster_of(const workloads::Registry& registry,
                                                 std::uint64_t ceiling_bytes);

/// The class of intensity a workload is scheduled as (see above).
enum class Intensity : std::uint8_t { kCompute, kMemory, kBalanced };

[[nodiscard]] Intensity classify(const std::string& workload);

struct MixedOptions {
  std::size_t workers = 0;
  std::uint64_t seed = 1;
  std::int64_t duration_ns = 0;  ///< The run's length: steps are made while they fall inside it.
  std::int64_t rotation_ns = 0;  ///< The interval between rotations.
  /// The most a worker's footprint is; datasets change size below it, by half.
  std::uint64_t footprint_bytes = 0;
  /// The CPU sets workers rotate across; empty: workers are never moved. If
  /// given, at least two, each a Rebind's list (strictly ascending).
  std::vector<std::vector<std::uint32_t>> placements;
  verification::DutyCycle duty;
};

/// The most steps a schedule may have: an interval so short that it would make
/// more is a mistake, and the schedule is held in memory.
inline constexpr std::size_t kMaxSteps = 100'000;

/// The most commands (a step's worker commands, summed over the schedule) one
/// may hold: a thousand workers over a hundred thousand steps would hold a
/// hundred million, which no run needs and no machine should be asked for.
inline constexpr std::size_t kMaxCommands = 4'000'000;

/// The schedule, or why the options make none: no workers, a duration or
/// rotation that is not positive, no footprint, too many steps or commands, one
/// set of CPUs or an invalid one, or no workload in the roster that fits the
/// footprint.
[[nodiscard]] core::Result<Schedule, std::string> make_mixed(const std::vector<RosterEntry>& roster,
                                                             const MixedOptions& options);

}  // namespace loadforge::mixed

#endif
