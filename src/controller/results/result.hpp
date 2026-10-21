// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_RESULTS_RESULT_HPP
#define LOADFORGE_CONTROLLER_RESULTS_RESULT_HPP

// CONTRACT (ADR 0004). The result of a run: the versioned schema every
// reporter renders and every later run is compared against (PLAN F12,
// IMPLEMENTATION M3). Frozen: changed only by a contract change, never inside
// a stream.
//
//   Provider of RunResult values and of derive_verdict(): the results stream
//     (src/controller/results/), which aggregates the run.
//   Consumers: the reporters (reporters/writer.hpp), explore's trials, the
//     comparison of one run with another.
//
// VERSIONED FROM THE FIRST RESULT
// -------------------------------
// kSchemaVersion is written into every rendering. A change to these types
// that changes what a rendering means increments it, in the contract change
// that makes it; a reader of an older result refuses a version it does not
// know rather than misreading it.
//
// NULLABLE, AND SAID SO
// ---------------------
// Every fingerprint field the machine may not tell us is optional; every
// metric is a summary or an Unavailable with its remedy; every bit field a
// bounded failure does not have is absent from its type. A reporter renders
// absence as the format's explicit absence -- JSON null, an empty CSV cell
// with the reason beside it -- never as zero (M3 exit criterion 3).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "controller/supervision.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "primitives/simd/dispatch.hpp"
#include "statistics/signature.hpp"
#include "telemetry/source.hpp"
#include "verification/isolation.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"
#include "workloads/workload.hpp"

namespace loadforge::results {

inline constexpr std::uint32_t kSchemaVersion = 3;

// Version 2 (contract change of the M3 corrections): a counter's summary carries
// `resets` (statistics/signature.hpp); the unit "microjoules" exists, and the
// power counter is in it; EDAC counters have a null `modulus` (a step down is a
// reset); and the verdict reads the blindness of the error counters.

// Version 3 (contract change of the M2 corrections): a record's provenance carries
// `physical_package`, `decomposition` and `footprint_bytes`, and `physical_core` is
// the core's id within its package; a workload carries `lock_policy` and
// `memory_lock` (what the runner did about locking the working set).

/// The run fingerprint (F12): the operating context two runs must share to be
/// compared. Every field the machine might not tell us is optional.
struct Fingerprint {
  std::string loadforge_version;
  std::optional<std::string> git_commit;
  std::string compiler;
  std::string flags;
  std::optional<std::string> cpu_model;
  std::optional<std::string> microcode;
  std::optional<std::string> kernel;
  std::optional<bool> smt_enabled;
  std::optional<std::string> power_source;  ///< "ac", "battery".
  std::optional<std::string> battery_state;
  std::optional<std::string> cpufreq_governor;
  std::optional<std::string> energy_performance_preference;
  std::optional<std::string> platform_profile;
  std::optional<bool> turbo_enabled;
  std::optional<std::string> cooling_profile;
  std::optional<std::uint64_t> cgroup_memory_limit_bytes;
  std::optional<std::string> huge_page_policy;
  std::optional<double> temperature_at_start_c;
  std::optional<fpenv::FpState> fp_environment;
  std::optional<simd::IsaPath> isa_path;
};

/// The newest reading of one series at or before the moment a finding
/// arrived: the "telemetry at time of error" of verification.md §8.
struct TelemetryAt {
  telemetry::Metric metric;
  telemetry::Point point;
};

/// One record, with what the controller attached to it (record.hpp, "two
/// stages of one record"): the isolation report when the record was a wrong
/// result and the ladder ran, and the telemetry at the time.
struct Finding {
  verification::ErrorRecord record;
  std::optional<verification::IsolationReport> isolation;
  std::vector<TelemetryAt> telemetry;
};

/// One workload's part of the run.
///
///   descriptor     the workload's own description, version and class.
///   isa, decomposition   the path and decomposition its units ran with.
///   duty           the verification duty cycle (F17).
///   tally          the runner's totals over every worker that ran it.
///   logical_bytes  units computed times the plan's logical bytes (F12).
///   findings       every record, in the order the controller received them.
///   lock_policy, memory_lock   the working set's lock: the choice and its outcome.
struct WorkloadResult {
  workloads::Descriptor descriptor;
  simd::IsaPath isa = simd::IsaPath::kScalar;
  std::uint32_t decomposition = 0;
  verification::DutyCycle duty;
  verification::Tally tally;
  std::uint64_t logical_bytes = 0;
  std::vector<Finding> findings;
  /// What the controller chose for the working set's lock, and what the
  /// worker's runner said came of it; absent when the worker never said (it
  /// did not get as far as a runner).
  verification::LockPolicy lock_policy = verification::LockPolicy::kRequire;
  std::optional<verification::MemoryLock> memory_lock;
};

/// One worker process, as supervision classified it.
struct WorkerResult {
  std::size_t slot = 0;
  controller::WorkerFate fate = controller::WorkerFate::kUnrecognised;
  std::optional<std::uint32_t> cpu;
  std::string termination;  ///< The raw fact behind the fate.
};

/// What was watching the run (F17): two runs are not compared without it.
struct Observers {
  std::int64_t sample_interval_ns = 0;
  std::optional<std::int64_t> journal_flush_interval_ns;
  std::optional<double> controller_overhead;  ///< Share of one CPU, 0 to 1.
};

/// The run's one-word answer, by derive_verdict().
///
///   kClean            everything was checked and nothing was wrong.
///   kCorrectedErrors  nothing was wrong, and the hardware corrected errors
///                     while it ran: not a clean run (F13).
///   kIncomplete       nothing was found wrong, and something was not checked
///                     or did not finish: "no errors" cannot be said. That
///                     includes a hardware error counter that was blind for part
///                     of the run (below): the errors it was to count may have
///                     happened there.
///   kFindings         something was computed wrong, or the hardware reported
///                     an uncorrected error.
enum class Verdict : std::uint8_t { kClean, kCorrectedErrors, kIncomplete, kFindings };

[[nodiscard]] constexpr std::string_view describe(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::kClean:
      return "clean";
    case Verdict::kCorrectedErrors:
      return "corrected errors";
    case Verdict::kIncomplete:
      return "incomplete";
    case Verdict::kFindings:
      return "findings";
  }
  return "unrecognised verdict";
}

/// The EDAC metric names the verdict reads (the edac source reports them,
/// per memory controller or DIMM, as counters).
inline constexpr std::string_view kCorrectedErrorsMetric = "edac.corrected";
inline constexpr std::string_view kUncorrectedErrorsMetric = "edac.uncorrected";

/// The whole run.
struct RunResult {
  std::uint32_t schema_version = kSchemaVersion;
  config::Mode mode = config::Mode::kStress;
  std::int64_t elapsed_ns = 0;
  Fingerprint fingerprint;
  telemetry::CapabilityReport capabilities;
  statistics::LoadSignature signature;
  std::vector<WorkloadResult> workloads;
  std::vector<WorkerResult> workers;
  Observers observers;
  Verdict verdict = Verdict::kIncomplete;
};

/// The verdict a run's contents imply. Provided by the results stream.
///
/// The first rule that holds decides:
///   1. kFindings when any finding's record is a wrong result, or any
///      signature metric named kUncorrectedErrorsMetric is a counter whose
///      delta is above zero.
///   2. kIncomplete when there is no workload; when any finding is a
///      VerificationNotPerformed; when any workload's tally has oracle_blocks
///      or integrity_checked at zero; when any worker's fate is not
///      clean (controller::is_clean); or when any signature metric named
///      kCorrectedErrorsMetric or kUncorrectedErrorsMetric was blind: it has
///      Unavailable readings in the run's window (`unavailable_samples` above
///      zero) or was reset (`resets` above zero). A counter that was there and
///      went blind is not a counter that did not move; a counter that was
///      absent at probe is not in the signature, and the capability report
///      says it was never read, which this rule does not call blind.
///   3. kCorrectedErrors when any signature metric named
///      kCorrectedErrorsMetric is a counter whose delta is above zero.
///   4. kClean.
[[nodiscard]] Verdict derive_verdict(const RunResult& result);

}  // namespace loadforge::results

#endif
