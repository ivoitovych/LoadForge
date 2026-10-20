// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_REPORT_HPP
#define LOADFORGE_CONTROLLER_REPORT_HPP

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "controller/safety.hpp"
#include "controller/supervision.hpp"
#include "core/duration.hpp"

namespace loadforge::controller {

/// What a run is about to do, for the banner.
struct RunPlan {
  config::Mode mode = config::Mode::kStress;
  core::Duration duration{0};
  std::size_t workers = 0;
  std::string executable;
  std::vector<LimitStatus> limits;
  /// The machine as the snapshot taken before the first fork describes it
  /// (topology::describe), or a statement that no snapshot was offered.
  std::string topology;

  [[nodiscard]] friend bool operator==(const RunPlan&, const RunPlan&) = default;
};

/// One worker's story, as the controller will tell it.
struct WorkerReport {
  std::size_t slot = 0;
  pid_t pid = 0;
  WorkerFate fate = WorkerFate::kUnrecognised;
  std::string termination;  ///< ExitStatus::describe(): the raw fact behind the fate.
  std::string stop;         ///< How far the controller got in stopping it.
  std::uint64_t progress = 0;
  std::vector<std::string> records;  ///< What it sent, one line each, in order.

  [[nodiscard]] friend bool operator==(const WorkerReport&, const WorkerReport&) = default;
};

/// The whole run.
///
/// `worker_exit_code` is set in exactly one situation: this process is a
/// forked child whose exec failed, and it must exit with that code at once
/// rather than carry on as a second controller. The caller returns it from
/// main; the child is single-threaded and the streams were flushed before
/// the fork, so an ordinary exit is safe and, unlike _exit, measurable.
struct RunReport {
  std::vector<WorkerReport> workers;
  std::int64_t elapsed_ns = 0;
  std::optional<int> worker_exit_code;

  [[nodiscard]] friend bool operator==(const RunReport&, const RunReport&) = default;
};

/// How many workers completed, and whether every one did. Free functions so
/// the report stays a plain aggregate.
[[nodiscard]] std::size_t clean_count(const RunReport& report) noexcept;
[[nodiscard]] bool all_clean(const RunReport& report) noexcept;

/// Where the controller tells what it sees, as it sees it. The console
/// implements it for a person; a test implements it to watch, and to act in
/// the middle of a run it could not otherwise reach into.
class Reporter {
 public:
  Reporter() = default;
  Reporter(const Reporter&) = delete;
  Reporter(Reporter&&) = delete;
  Reporter& operator=(const Reporter&) = delete;
  Reporter& operator=(Reporter&&) = delete;
  virtual ~Reporter() = default;

  virtual void run_starting(const RunPlan& plan) = 0;
  /// One thing that happened, `elapsed_ns` into the run.
  virtual void event(std::int64_t elapsed_ns, std::string_view line) = 0;
  virtual void run_finished(const RunReport& report) = 0;
};

}  // namespace loadforge::controller

#endif
