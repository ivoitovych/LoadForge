// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_RERUN_HPP
#define LOADFORGE_CONTROLLER_RERUN_HPP

// The provider of verification::Rerunner (verification/isolation.hpp): the
// re-run ladder's way of running one unit again, in a worker process of its
// own pinned to the CPU a rung names, with the thread count it names and every
// check on.
//
// A PROCESS, NOT A THREAD
// -----------------------
// The unit is run by this binary re-executed with --rerun (worker/entry.hpp),
// for the reasons the workers are: a fresh address space (the original may
// have run in memory that went wrong), a placement that is the kernel's and
// not the controller's say-so, and a crash that is a crash of the re-run and
// not of the controller. What came of it is the exit code (RerunExit), and
// nothing else crosses: a re-run that dies by a signal is NOT a clean re-run
// and not a reproduction either, it is a re-run that could not be made,
// because a signal names a fault and not its cause (EXT-03).
//
// WHAT THE RECORD DOES NOT SAY
// ----------------------------
// A record's provenance names the workload, its version, the unit's index,
// the seed and the ISA path, which is enough to regenerate the INPUT anywhere
// (record.hpp). It does not name the footprint or the decomposition, and a
// BitExactFixedDecomposition workload's OUTPUT depends on the decomposition.
// Those are the run's, which the controller knows; use() tells the re-runner
// which run's findings it is about to be asked to repeat.

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>

#include "controller/report.hpp"
#include "platform/syscalls.hpp"
#include "verification/isolation.hpp"

namespace loadforge::controller {

/// A Rerunner that can also say it is, after a failed exec, the CHILD of a
/// fork: the process that must exit with the launcher's failure code and not
/// carry on as a second controller (the same rule Controller::run follows for
/// a worker).
class ChildAwareRerunner : public verification::Rerunner {
 public:
  /// The run the next findings come from: footprint, decomposition and line
  /// size are the run's, not the record's.
  virtual void use(const WorkerWork& work) = 0;

  /// Set once this process is a forked child whose exec failed; the caller
  /// returns it from main. Every later rerun() answers kNotPerformed.
  [[nodiscard]] virtual std::optional<int> child_exit_code() const noexcept = 0;
};

struct RerunOptions {
  /// How long one re-run may take before it is ended. A unit is milliseconds
  /// of work; a re-run that takes this long is not running it.
  std::int64_t timeout_ns = 60'000'000'000;
};

class RealRerunner final : public ChildAwareRerunner {
 public:
  RealRerunner(platform::Syscalls& syscalls, std::string executable, RerunOptions options);

  void use(const WorkerWork& work) override;
  [[nodiscard]] verification::RerunOutcome rerun(
      const verification::WrongResult& finding, const verification::RerunRequest& request) override;
  [[nodiscard]] std::optional<int> child_exit_code() const noexcept override;

 private:
  platform::Syscalls* syscalls_;
  std::string executable_;
  RerunOptions options_;
  std::optional<WorkerWork> work_;
  std::optional<int> child_exit_;
};

}  // namespace loadforge::controller

#endif
