// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_EXPLORE_COMMAND_HPP
#define LOADFORGE_MAIN_EXPLORE_COMMAND_HPP

// `loadforge explore` (docs/PLAN.md F5, IMPLEMENTATION M6): the worst-case search
// of src/explore, run on this machine through ControllerTrialRunner.
//
//   loadforge explore --objective METRIC[@INDEX]:STATISTIC:higher|lower
//                     --candidate ID:WORKLOAD[+WORKLOAD...]:THREADS[:CPUS] ...
//                     [--reference ID:WORKLOAD[+WORKLOAD...]:THREADS[:CPUS]]
//                     [--duration D] [--settle METRIC [--settle-slope PER_SECOND]
//                     [--settle-window D] [--settle-max D]]
//                     [--replicates N] [--final-replicates N] [--elimination N]
//                     [--interleave N] [--reference-replicates N]
//                     [--seed N] [--footprint SIZE] [--isa PATH] [--lock POLICY]
//                     [--sample-interval D] [--verbose]
//
// The objective is bound against what this machine can read before any worker
// starts: a metric it cannot read is refused, with the source's own reason and
// remedy, and nothing runs. Every number the search reports carries its
// replicate count, spread and interval, and every replicate is listed.
//
// Exit codes, in the vocabulary of `stress` (outcome.hpp):
//   0  a report was produced, and every candidate was scored
//   1  refused before the first trial: the objective, the search or a candidate
//      cannot be run on this machine, or the machine cannot be described
//   2  usage error
//   3  a trial could not be run, or the reference could not be used; or the
//      temperature ceiling (--max-temperature) ended a trial, which stops the search
//   5  a candidate's trial produced findings: a finding, not a score
//   6  a candidate's trial did not finish or compare everything, or a trial held no
//      reading of the objective: a check was not made
// When several apply the first of 5, 6 is the code of a search that ended; a
// search that did not end is 1, 3 or 6 by the fault that ended it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "explore/objective.hpp"
#include "explore/search.hpp"
#include "main/trial_runner.hpp"
#include "verification/runner.hpp"

namespace loadforge::cli {

/// The command line, parsed.
///
///   reference   none: the first candidate's workloads, one worker each, anywhere.
///   settle      none: no settle phase. Applies to every candidate and the reference.
///   The search parameters default to explore::SearchConfig's.
struct ExploreArgs {
  explore::Objective objective;
  std::vector<explore::Candidate> candidates;
  std::optional<explore::Candidate> reference;
  std::int64_t duration_ns = 10'000'000'000;
  std::optional<explore::Settle> settle;
  std::size_t base_replicates = 1;
  std::size_t elimination_factor = 2;
  std::size_t final_replicates = 3;
  std::size_t interleave_every = 4;
  std::size_t reference_replicates = 2;
  std::uint64_t seed = 1;
  std::uint64_t footprint_bytes = std::uint64_t{32} << 20U;
  std::optional<simd::IsaPath> isa;
  verification::LockPolicy lock = verification::LockPolicy::kTry;
  std::int64_t sample_interval_ns = 1'000'000'000;
  /// --max-temperature: every trial's ceiling, in degrees Celsius. A trial in
  /// which a sensor reads this or more is stopped like a stress run and fails
  /// the search (3); with no sensor to read, nothing starts (1).
  std::optional<std::int64_t> max_temperature_c;
  bool verbose = false;
};

/// The arguments after "explore". A refusal names the option.
[[nodiscard]] core::Result<ExploreArgs, std::string> parse_explore_args(
    std::span<const std::string_view> args);

/// The search the arguments describe: the duration, the settle phase and the
/// duty cycle are the same for every candidate and the reference.
[[nodiscard]] explore::SearchConfig search_config_of(const ExploreArgs& args);

/// A finished search in words: the objective, the worst case with its replicate
/// count, spread and interval, how it ranks against the best rival, every
/// candidate's replicates and fate, and every reading of the reference.
[[nodiscard]] std::string render_report(const explore::SearchReport& report);

/// The exit code of a search that ended with a report.
[[nodiscard]] int exit_code_for(const explore::SearchReport& report) noexcept;

/// The exit code of a search that ended in `error`.
[[nodiscard]] int exit_code_for(const explore::ExploreError& error) noexcept;

/// The search on `runner`: the report on `out`, or the reason on `err`. A trial
/// that came back in a forked worker whose exec failed ends the search and
/// answers that worker's exit code, saying nothing.
[[nodiscard]] int run_explore(ControllerTrialRunner& runner, explore::SearchConfig config,
                              std::ostream& out, std::ostream& err);

/// The command: `args` start after "explore". `base` supplies what a test
/// replaces (the machine's roots, the sources, the summarizer); the executable
/// and the options the arguments name are set over it.
[[nodiscard]] int explore_command(std::span<const std::string_view> args,
                                  const std::string& self_link, std::string_view sysfs_root,
                                  const TrialOptions& base, std::ostream& out, std::ostream& err);

}  // namespace loadforge::cli

#endif
