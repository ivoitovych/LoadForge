// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_ISOLATE_HPP
#define LOADFORGE_CONTROLLER_ISOLATE_HPP

// Fault isolation over a finished run's findings (docs/verification.md §7):
// every wrong result the workers sent goes up the re-run ladder, one finding
// at a time, once the workers are gone -- so a re-run is never competing with
// the run it is about, and the reap that waits for a re-run's end waits for
// nothing else.
//
// WHICH FINDINGS ARE NOT ISOLATED, AND WHY EACH SAYS SO
// -----------------------------------------------------
//   * a check that was not made: nothing is wrong, so there is nothing to
//     repeat (isolation.hpp, "only wrong results enter");
//   * a result from a worker that ran on several CPUs: the ladder's first
//     rung is "the original CPU", and a unit spread over an executor ran on
//     all of them -- the record names none (journal fragment 11);
//   * every one past the limit, counted per worker so that a fault on one
//     core does not starve the other workers' findings of theirs: a machine
//     with a persistent fault finds one per unit, and the ladder is three
//     processes a rung;
//   * a worker with no topology: the other cores cannot be named.
// Each is stated on the finding as absence, never as an isolation that was
// not made.

#include <cstddef>
#include <optional>

#include "controller/report.hpp"
#include "controller/rerun.hpp"
#include "topology/snapshot.hpp"
#include "verification/isolation.hpp"

namespace loadforge::controller {

inline constexpr std::size_t kDefaultIsolationLimit = 16;

struct IsolationSummary {
  std::size_t isolated = 0;
  /// Set when this process turned out to be a forked child whose exec failed
  /// (ChildAwareRerunner): it must exit with this code at once, and the
  /// isolation stopped where it was.
  std::optional<int> child_exit_code;
};

/// Isolates up to `limit` wrong results of each worker in `report`, writing each result into
/// the finding it is about.
[[nodiscard]] IsolationSummary isolate_findings(RunReport& report,
                                                const topology::Snapshot* machine,
                                                verification::Isolator& isolator,
                                                ChildAwareRerunner& rerunner, std::size_t limit);

}  // namespace loadforge::controller

#endif
