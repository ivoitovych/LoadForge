// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_VERIFIED_RUN_HPP
#define LOADFORGE_WORKER_VERIFIED_RUN_HPP

// A worker running a real workload: the null workload's loop with the work
// inside it (scheduler.hpp). Each tick publishes the beat, judges the
// controller, looks at the control pipe, and then runs one slice of units
// through the input-integrity runner (verification/runner.hpp), sending every
// record the slice made on the record channel and the running tally with it.
//
// THE ORDER OF ONE TICK
// ---------------------
// As the null workload's, with the slice where its wait was: the beat first,
// carrying the units computed so far as its progress (so a worker that is
// computing is told from one that is alive and stuck), then the duration, then
// the controller, then the control pipe LOOKED AT and not waited on -- a
// worker with work to do has nothing to wait for -- and then the slice. A
// slice is sized from the time the last one took so that a tick lasts about
// half a heartbeat interval: the runner's own words (runner.hpp) for what the
// worker owes its controller.
//
// WHAT HAPPENS BEFORE THE FIRST TICK, AND WHY IN THIS ORDER
// ---------------------------------------------------------
//   1. the floating-point environment is established and asserted, before any
//      thread exists: the executor's threads assert that they inherited the
//      creator's state (docs/determinism.md §3), so the state must be right
//      before they are made;
//   2. the ISA path the controller chose is checked against what this CPU can
//      run and this build holds, and used or refused, never replaced;
//   3. the workload, then its oracle for the plan the workload resolved, then
//      the executor, then the runner;
//   4. the environment is asserted again, after all of that: the point at
//      which a library a workload pulled in could have changed it. A change
//      is a configuration error, reported, not repaired -- a register repaired
//      silently would hide the third party that wrote it.
// Any refusal is a WorkerError and the worker's own setup failure.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "platform/affinity.hpp"
#include "platform/syscalls.hpp"
#include "verification/oracle.hpp"
#include "verification/runner.hpp"
#include "worker/entry.hpp"
#include "worker/executor.hpp"
#include "worker/executor/thread_pool.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"
#include "worker/services.hpp"
#include "workloads/workload.hpp"

namespace loadforge::worker {

/// The most units one slice runs, however fast they are: a slice must end
/// within a heartbeat interval even if the unit time measured was a fluke.
inline constexpr std::uint64_t kMaxSliceUnits = std::uint64_t{1} << 20U;

/// The next slice's size, from the last one's and the time it took: the size
/// that would have taken `target_ns`, at most double the last (a slice that
/// happened to be fast does not license a burst) and at least one unit. A slice
/// that measured no time at all doubles.
[[nodiscard]] std::uint64_t next_slice_units(std::uint64_t last_units, std::int64_t took_ns,
                                             std::int64_t target_ns) noexcept;

/// One thread placement per thread: thread t on cpus[t mod cpus.size()] alone,
/// or -- with no CPUs named -- left where the creating thread may run. A CPU
/// no mask can name is the refusal's text.
[[nodiscard]] core::Result<std::vector<ThreadPlacement>, std::string> placements_for(
    std::uint32_t threads, const std::vector<std::uint32_t>& cpus);

/// What a verified run did.
struct VerifiedOutcome {
  Outcome outcome;
  verification::Tally tally;
  std::string note;  ///< Why the runner stopped itself, when it did. Else empty.
};

/// Everything a unit of work needs, in the order it is destroyed in reverse.
struct Prepared {
  std::unique_ptr<workloads::Workload> workload;
  std::unique_ptr<verification::Oracle> oracle;
  std::unique_ptr<Executor> executor;
  std::unique_ptr<verification::SliceRunner> runner;
};

/// Steps 1 to 4 above for `args`, on the CPUs in `cpus`, starting the run at
/// `first_index`. Shared by the worker and the re-run.
[[nodiscard]] core::Result<Prepared, WorkerError> prepare(platform::Syscalls& syscalls,
                                                          const WorkloadArgs& args,
                                                          const std::vector<std::uint32_t>& cpus,
                                                          std::uint64_t first_index,
                                                          const Services& services);

class VerifiedRun {
 public:
  VerifiedRun(platform::Syscalls& syscalls, ipc::RecordWriter& records, Session session,
              const WorkerArgs& args, const Services& services) noexcept
      : syscalls_(&syscalls),
        records_(&records),
        session_(session),
        args_(&args),
        services_(&services) {}

  /// Runs until the duration elapses, the controller asks for a stop or goes,
  /// or the runner stops itself. A record the channel will not take is the
  /// controller gone (EPIPE) or the worker's own failure (anything else).
  [[nodiscard]] core::Result<VerifiedOutcome, WorkerError> run();

 private:
  platform::Syscalls* syscalls_;
  ipc::RecordWriter* records_;
  Session session_;
  const WorkerArgs* args_;
  const Services* services_;
};

}  // namespace loadforge::worker

#endif
