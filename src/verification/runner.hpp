// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_RUNNER_HPP
#define LOADFORGE_VERIFICATION_RUNNER_HPP

// CONTRACT (ADR 0004). How the worker runs a workload: in slices, every unit
// inside the input-integrity bracket of verification.md §4a, every sampled
// unit checked by the independent oracle, every finding an error record.
// Frozen: changed only by a contract change, never inside a stream.
//
//   Provider: the input-integrity stream (src/verification/integrity/), which
//     owns the bracket and the duty cycle -- the F18 logic, written once
//     rather than in every workload.
//   Consumers: the worker loop, which calls run_slice() between heartbeats
//     and sends the records; fault isolation's re-runs, which run one unit.
//
// ONE UNIT, IN ORDER
// ------------------
// For unit k (k = first_index, first_index + 1, ...), in slot k mod
// plan().units of the working set:
//
//   1. write: generate(k) into the slot's input.
//   2. open:  if k is bracketed, regenerate k into a separate buffer and
//             compare it with the slot's input. A difference is an
//             ExactBitCorruption, Side::kInput, kCorruptBeforeConsumption,
//             Domain::kStored -- and the unit stops here: it is neither
//             computed nor checked, because its input is already known wrong.
//   3. compute the unit into the slot's output.
//   4. check: if k is sampled, give the oracle the slot's input and output.
//             Agreement adds to the tally; ExactMismatch and BoundedMismatch
//             become ExactBitCorruption (Side::kOutput) and
//             NumericalVerificationFailure; NotChecked becomes
//             VerificationNotPerformed.
//   5. close: if k is bracketed, regenerate and compare again. A difference
//             is an ExactBitCorruption, Side::kInput,
//             kCorruptDuringConsumption: the input went wrong while the unit
//             was being computed.
//
// The slot's input is not written between steps 1 and 5, so the governing
// invariant holds: an input stays verifiable until every result derived from
// it has been checked (§4a). Every record of unit k carries
// provenance.input_integrity -- kIntact when both checks passed, the failing
// one's value when one did, kNotChecked when k was not bracketed.
//
// WHICH UNITS
// -----------
// Unit k is sampled by the oracle when duty.oracle_period != 0 and
// k % duty.oracle_period == 0, and bracketed when duty.integrity_period != 0
// and k % duty.integrity_period == 0. A rule, not a random draw, so a run's
// tally is predictable and a re-run checks the same units.
//
// WHEN A CHECK HAPPENS
// --------------------
// In the slice that computed the unit, always: no result outlives its slice
// unverified, which keeps the §4a invariant trivially. Benchmark mode keeps
// expensive checks out of its measurement by a lower duty cycle, reported
// with the signature -- F17's stated fallback. Deferring checks to between
// windows would need results held across slices; that is a contract change
// for the day a measurement asks for it (ADR 0004).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "verification/record.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::platform {
class Syscalls;
}  // namespace loadforge::platform

namespace loadforge::verification {

/// What a slice, or a whole run, did.
///
///   units_computed        units that reached step 3.
///   oracle_checked        units given to the oracle (step 4).
///   oracle_blocks         blocks the oracle compared, summed over verdicts.
///   oracle_failed         units whose verdict was a mismatch.
///   oracle_not_performed  units whose verdict was NotChecked.
///   integrity_checked     units bracketed (step 2 reached).
///   integrity_failed      units whose opening or closing check failed.
struct Tally {
  std::uint64_t units_computed = 0;
  std::uint64_t oracle_checked = 0;
  std::uint64_t oracle_blocks = 0;
  std::uint64_t oracle_failed = 0;
  std::uint64_t oracle_not_performed = 0;
  std::uint64_t integrity_checked = 0;
  std::uint64_t integrity_failed = 0;
};

[[nodiscard]] inline bool operator==(const Tally& lhs, const Tally& rhs) noexcept {
  return std::tie(lhs.units_computed, lhs.oracle_checked, lhs.oracle_blocks, lhs.oracle_failed,
                  lhs.oracle_not_performed, lhs.integrity_checked, lhs.integrity_failed) ==
         std::tie(rhs.units_computed, rhs.oracle_checked, rhs.oracle_blocks, rhs.oracle_failed,
                  rhs.oracle_not_performed, rhs.integrity_checked, rhs.integrity_failed);
}

/// The sum of two tallies, field by field.
[[nodiscard]] constexpr Tally operator+(const Tally& lhs, const Tally& rhs) noexcept {
  return Tally{lhs.units_computed + rhs.units_computed,
               lhs.oracle_checked + rhs.oracle_checked,
               lhs.oracle_blocks + rhs.oracle_blocks,
               lhs.oracle_failed + rhs.oracle_failed,
               lhs.oracle_not_performed + rhs.oracle_not_performed,
               lhs.integrity_checked + rhs.integrity_checked,
               lhs.integrity_failed + rhs.integrity_failed};
}

/// One slice: how many units to run. Zero is allowed and does nothing.
struct SliceRequest {
  std::uint64_t units = 0;
};

/// What one slice, or finish(), found.
///
///   tally       this slice's counts only.
///   records     every record the slice made, ordered by unit index, and
///               within a unit in step order (open, check, close).
///   next_index  the index the next slice starts at: the run's progress
///               counter, which the worker publishes in its heartbeat.
struct SliceReport {
  Tally tally;
  std::vector<ErrorRecord> records;
  std::uint64_t next_index = 0;
};

/// Why a runner refused.
///
///   kConfiguration  the setup is wrong: a missing workload, oracle, executor
///                   or system-call seam; an oracle for another workload; a
///                   line size that is not a power of two of at least 8.
///   kResources      the working set could not be allocated or placed. The
///                   detail names the step (platform/memory.hpp's words).
///   kFinished       called after finish().
enum class RunnerFault : std::uint8_t { kConfiguration, kResources, kFinished };

[[nodiscard]] constexpr std::string_view describe(RunnerFault fault) noexcept {
  switch (fault) {
    case RunnerFault::kConfiguration:
      return "runner configuration";
    case RunnerFault::kResources:
      return "runner resources";
    case RunnerFault::kFinished:
      return "runner already finished";
  }
  return "unrecognised runner fault";
}

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct RunnerError {
  RunnerFault fault = RunnerFault::kConfiguration;
  std::string detail;
};

[[nodiscard]] inline bool operator==(const RunnerError& lhs, const RunnerError& rhs) noexcept {
  return std::tie(lhs.fault, lhs.detail) == std::tie(rhs.fault, rhs.detail);
}

/// Runs one workload's units in slices. Created by make_slice_runner().
///
/// run_slice(request) runs units next_index .. next_index + request.units - 1
/// by the steps in the header comment and returns the slice's report. It
/// returns an error only for kFinished: a hardware finding is a record, never
/// an error, and a tool failure inside a check is a VerificationNotPerformed.
/// It does no work but the units' and blocks on nothing else, so its time is
/// proportional to request.units -- the worker sizes slices from measured
/// time to keep its heartbeat fresh.
///
/// finish() ends the run and returns a final report with no units in it.
/// Its records state what the whole run did not check, judged over every
/// slice (verification.md §5: comparing zero is an error):
///   * a VerificationNotPerformed for the oracle when the total oracle_blocks
///     is zero -- what: "oracle over <workload> units", why: the duty cycle
///     and the units computed, blocks_compared: 0;
///   * a VerificationNotPerformed for the bracket when the total
///     integrity_checked is zero -- what: "input-integrity bracket over
///     <workload> units".
/// Both are made even for a run that computed no unit at all. After finish(),
/// run_slice() and finish() answer kFinished.
///
/// total() is the sum of every report returned so far.
///
/// Stopping: there is no stop call. The worker stops by not calling
/// run_slice() again; a slice in progress completes, because its time is
/// bounded by its size.
///
/// Thread-safety: one thread at a time. The runner uses the executor while a
/// slice runs, and nothing else may use it then.
class SliceRunner {
 public:
  SliceRunner() = default;
  SliceRunner(const SliceRunner&) = delete;
  SliceRunner(SliceRunner&&) = delete;
  SliceRunner& operator=(const SliceRunner&) = delete;
  SliceRunner& operator=(SliceRunner&&) = delete;
  virtual ~SliceRunner() = default;

  [[nodiscard]] virtual core::Result<SliceReport, RunnerError> run_slice(
      const SliceRequest& request) = 0;
  [[nodiscard]] virtual core::Result<SliceReport, RunnerError> finish() = 0;
  [[nodiscard]] virtual const Tally& total() const noexcept = 0;
};

/// Everything a runner is built from. The pointers are borrowed and must
/// outlive the runner.
///
///   workload, oracle, executor   what to run, what checks it, what it runs on.
///   syscalls     the seam the working set is allocated through.
///   duty         the duty cycle; recorded in every record's provenance.
///   line_bytes   the topology's cache-line size: the alignment of every
///                slot's input and output, and every ExactBitCorruption's
///                line_bytes.
///   base         the provenance every record starts from: where the worker
///                runs (logical_cpu, physical_core, numa_node), the
///                workload's name and version, the seed and the ISA path.
///                iteration, buffer_address, buffer_offset, input_integrity
///                and duty are the runner's to set, per record.
///   first_index  the first unit's index.
struct RunnerSetup {
  const workloads::Workload* workload = nullptr;
  const Oracle* oracle = nullptr;
  worker::Executor* executor = nullptr;
  platform::Syscalls* syscalls = nullptr;
  DutyCycle duty;
  std::uint32_t line_bytes = 64;
  Provenance base;
  std::uint64_t first_index = 0;
};

/// A runner for `setup`, or kConfiguration or kResources. Provided by the
/// input-integrity stream.
[[nodiscard]] core::Result<std::unique_ptr<SliceRunner>, RunnerError> make_slice_runner(
    const RunnerSetup& setup);

}  // namespace loadforge::verification

#endif
