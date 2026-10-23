// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_OUTCOME_HPP
#define LOADFORGE_MAIN_OUTCOME_HPP

// What a finished run is, to the shell and to the results: the controller's
// report put into the results' types, and the verdict they derive from it
// turned into the process's exit code.
//
// The controller has no dependency on the results module (the results link the
// controller, not the other way), so this is where the two meet.

#include <cstddef>
#include <functional>

#include "config/config.hpp"
#include "controller/isolate.hpp"
#include "controller/report.hpp"
#include "controller/results/result.hpp"
#include "topology/snapshot.hpp"
#include "verification/isolation.hpp"

namespace loadforge::cli {

/// The exit codes of a stress run that ended and was reported. M2 and M3 each
/// assigned 4 and 5 and meant different things by them; main already said what
/// 4 and 5 are (M3), so they stay, and the wrong result M2 reported as 4 is a
/// fault, which is 5 (docs/journal/11-integration-m2.md).
///
///   0  clean: every worker completed, nothing was wrong, and every verifying
///      worker's oracle and input bracket compared something.
///   3  a worker did not complete.
///   4  the run ended and a result file could not be written.
///   5  a fault: a wrong result, or an error the hardware reported, corrected
///      or not (the verdict is findings or corrected errors). It outranks
///      every other, 3 and 4 included: it is the finding.
///   6  nothing was wrong that was seen, and something was not seen: a check
///      was not performed, or a verifying worker never compared anything. A
///      run that checked nothing is not a pass (verification.md §5).
///   7  the temperature ceiling (`limits.max_temperature_c`) ended the run: a
///      sensor read at or above it, or no sensor could be read any more and the
///      ceiling could not be enforced. The workers were stopped, then killed past
///      the grace, so their fates (3) and the checks they did not get to make (6)
///      are consequences of the stop and 7 says why; and it is a fact about the
///      machine that a failed write (4) must not hide. Only a fault (5), the
///      finding, outranks it: a wrong result found on the way is still the thing
///      to read first.
///   130 / 143  a signal (SIGINT, SIGTERM) asked for the end: the run was
///      stopped as at its deadline, reported, and its result written. It ranks
///      below a fault (5), the ceiling (7) and a failed write (4), which tell the
///      user what the signal alone does not, and above 3 and 6, which it explains:
///      a stopped worker that had to be killed, and checks cut short. The code is
///      what the shell would have said had the signal ended the process, so that
///      a script that tests for 130 still sees the interrupt.
/// When several apply the first of 5, 7, 4, 130/143, 3, 6 is the code. A run of nothing
/// but null workloads (M1) has nothing to verify and is 0 or 3 by its workers.
/// (A configured ceiling that cannot be read before the run is not 7: nothing
/// was started, and that is a refusal, 1.)
inline constexpr int kExitUnclean = 3;
inline constexpr int kExitOutputFailed = 4;
inline constexpr int kExitFault = 5;
inline constexpr int kExitWrongResult = kExitFault;
inline constexpr int kExitNotVerified = 6;
inline constexpr int kExitTemperature = 7;
/// A run a signal stopped exits with the shell's code for dying of that
/// signal, 128 plus its number: 130 for SIGINT, 143 for SIGTERM (journal 34b).
inline constexpr int kExitSignalBase = 128;

/// The code of a run whose verdict is known. `verified` is whether any worker
/// ran a verified workload: only then is an incomplete verdict a check not
/// made, and only then 6. Without one (the null workload) an incomplete verdict
/// is what a run with nothing to check always is. `temperature_stopped` is
/// whether the ceiling ended the run; `stop_signal` is the signal that stopped
/// it, or 0.
[[nodiscard]] int exit_code_for(results::Verdict verdict, bool workers_clean, bool verified = false,
                                bool temperature_stopped = false, int stop_signal = 0) noexcept;

/// Whether any worker of the report ran a verified workload.
[[nodiscard]] bool has_verified_worker(const controller::RunReport& report) noexcept;

/// `code` once a result file could not be written: a fault, and a stop by the
/// temperature ceiling, still outrank the failure to write about them.
[[nodiscard]] int with_output_failure(int code) noexcept;

/// The report in the results' types: one WorkloadResult per verifying worker,
/// one WorkerResult per worker. The telemetry, signature and fingerprint are
/// left as they are; they are M3's.
[[nodiscard]] results::RunResult to_run_result(const controller::RunReport& report,
                                               const config::Config& config);

[[nodiscard]] int exit_code_for(const controller::RunReport& report, const config::Config& config);

/// What follows a run whose workers are gone, and then `conclude`, whose value
/// is the exit code. When any wrong result was found, repeats up to `limit` of
/// each worker's in processes of their own, writes what that shows into the
/// report, and tells the reporter. A re-run whose exec failed in this process
/// is a failed child: its exit code is returned at once, `conclude` is not
/// called and nothing is reported. The command line concludes by assembling
/// and writing the result (M3), which needs the isolation in the report.
[[nodiscard]] int after_isolation(controller::RunReport& report, const topology::Snapshot* machine,
                                  verification::Isolator& isolator,
                                  controller::ChildAwareRerunner& rerunner, std::size_t limit,
                                  controller::Reporter& reporter,
                                  const std::function<int()>& conclude);

/// What follows a run whose workers are gone, and the process's exit code.
/// When any wrong result was found, repeats up to `limit` of each worker's in
/// processes of their own, writes what that shows into the report, and tells
/// the reporter. A re-run whose exec failed in this process is a failed child:
/// its exit code is returned at once, and nothing is reported.
[[nodiscard]] int finish_run(controller::RunReport& report, const config::Config& config,
                             const topology::Snapshot* machine, verification::Isolator& isolator,
                             controller::ChildAwareRerunner& rerunner, std::size_t limit,
                             controller::Reporter& reporter);

}  // namespace loadforge::cli

#endif
