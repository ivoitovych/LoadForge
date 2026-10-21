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

#include "config/config.hpp"
#include "controller/isolate.hpp"
#include "controller/report.hpp"
#include "controller/results/result.hpp"
#include "topology/snapshot.hpp"
#include "verification/isolation.hpp"

namespace loadforge::cli {

/// The exit code of a stress run that ended and was reported.
///
///   0  clean: every worker completed, nothing was wrong, and every verifying
///      worker's oracle and input bracket compared something.
///   3  a worker did not complete.
///   4  a wrong result was found. Outranks 3 and 5: it is the finding.
///   5  nothing was wrong that was seen, and something was not seen: a check
///      was not performed, or a verifying worker never compared anything. A
///      run that checked nothing is not a pass (verification.md §5).
/// A run of nothing but null workloads (M1) has nothing to verify and is 0 or
/// 3 by its workers alone.
inline constexpr int kExitUnclean = 3;
inline constexpr int kExitWrongResult = 4;
inline constexpr int kExitNotVerified = 5;

/// The report in the results' types: one WorkloadResult per verifying worker,
/// one WorkerResult per worker. The telemetry, signature and fingerprint are
/// left as they are; they are M3's.
[[nodiscard]] results::RunResult to_run_result(const controller::RunReport& report,
                                               const config::Config& config);

[[nodiscard]] int exit_code_for(const controller::RunReport& report, const config::Config& config);

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
