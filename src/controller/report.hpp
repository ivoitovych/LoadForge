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
#include "primitives/simd/dispatch.hpp"
#include "verification/isolation.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"
#include "workloads/workload.hpp"

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
  /// One clause per worker: what it runs, where, on which path (plan.hpp).
  /// A worker that runs the null workload says so.
  std::vector<std::string> workloads;
  /// The floating-point state asserted at startup, and the ISA paths chosen,
  /// in words; empty when the run asserts none (M1).
  std::string environment;
  /// The capability report (telemetry/capability), as the console prints it;
  /// empty when no source was offered.
  std::string capabilities;

  [[nodiscard]] friend bool operator==(const RunPlan&, const RunPlan&) = default;
};

/// What a worker was told to run, as the report keeps it: the workload's own
/// description of itself, and the choices the controller made for it. What
/// the results need to name the workload, and what isolation needs to place a
/// re-run.
struct WorkerWork {
  workloads::Descriptor descriptor;
  simd::IsaPath isa = simd::IsaPath::kScalar;
  std::uint32_t decomposition = 0;
  verification::DutyCycle duty;
  std::uint64_t logical_bytes = 0;  ///< Per unit (Plan::logical_bytes).
  std::uint32_t threads = 0;
  std::optional<std::uint32_t> cpu;  ///< Set when the worker was placed on one CPU.
  std::uint64_t seed = 0;
  std::uint64_t footprint_bytes = 0;
  std::uint32_t line_bytes = 0;
};

[[nodiscard]] bool operator==(const WorkerWork& lhs, const WorkerWork& rhs) noexcept;

/// A record a worker sent, as the controller holds it: the record, and what
/// fault isolation made of it. A record that is not a wrong result is never
/// isolated (isolation.hpp), and says so here.
struct FindingReport {
  verification::ErrorRecord record;
  std::optional<verification::IsolationReport> isolation;
  /// Why there is no isolation: "not a wrong result", "ran on several CPUs",
  /// "over the limit". Empty while isolation is still to be done or was done.
  std::string not_isolated;
};

[[nodiscard]] bool operator==(const FindingReport& lhs, const FindingReport& rhs) noexcept;

/// One worker's story, as the controller will tell it.
struct WorkerReport {
  std::size_t slot = 0;
  pid_t pid = 0;
  WorkerFate fate = WorkerFate::kUnrecognised;
  std::string termination;  ///< ExitStatus::describe(): the raw fact behind the fate.
  std::string stop;         ///< How far the controller got in stopping it.
  std::uint64_t progress = 0;
  std::vector<std::string> records;  ///< What it sent, one line each, in order.
  /// What a verifying worker did: its last tally, cumulative (worker/frames.hpp),
  /// and whether it ever sent one. A worker that sent none has an empty tally
  /// that says nothing about what it checked.
  verification::Tally tally{};
  bool tally_seen = false;
  /// What this worker ran, when it ran a real workload.
  std::optional<WorkerWork> work{};
  /// Every error record it sent, in order, up to kMaxFindingsKept; the counts
  /// below continue past that.
  std::vector<FindingReport> findings{};
  std::uint64_t wrong_results = 0;
  std::uint64_t not_performed = 0;
  std::uint64_t findings_dropped = 0;

  [[nodiscard]] friend bool operator==(const WorkerReport&, const WorkerReport&) = default;
};

/// The most error records the controller keeps for one worker. A machine with
/// a persistent fault makes one per unit, for as long as it runs; the counts
/// are exact and the first of them are kept.
inline constexpr std::size_t kMaxFindingsKept = 256;

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

/// The run's findings in total, over every worker: wrong results, checks that
/// were not performed.
[[nodiscard]] std::uint64_t wrong_result_count(const RunReport& report) noexcept;
[[nodiscard]] std::uint64_t not_performed_count(const RunReport& report) noexcept;

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
  /// Fault isolation has run over the report's findings (isolate.hpp) and the
  /// report now carries what it found. Called once, after run_finished, only
  /// when a run had a wrong result to isolate.
  virtual void isolation_finished(const RunReport& report) = 0;
};

}  // namespace loadforge::controller

#endif
