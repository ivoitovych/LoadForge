// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_SELFTEST_COMMAND_HPP
#define LOADFORGE_MAIN_SELFTEST_COMMAND_HPP

// `loadforge selftest` (docs/verification.md §9): what to run before a long run, to learn in
// seconds whether this build on this machine can give a hardware verdict at all.
//
// Two parts, one line per check, then a verdict:
//
//   1. the golden part: the digest `--selftest-digest` prints, compared with the digest this
//      source tree expects (main/selftest.hpp), in its two halves so that a mismatch says which
//      half differed: the configuration-parsing corpus, or the scalar kernels of the workloads.
//   2. the oracle part: for every workload in the registry, a few units on the path this CPU
//      would use and, when that is another path, on the scalar path too, each checked by the
//      workload's independent oracle through the code a stress run uses (worker::prepare and
//      the input-integrity runner); no second implementation of either.
//
// Exit codes: 0 every check passed; 8 (kExitSelftestFailed, outcome.hpp) at least one failed;
// 1 a refusal (an --isa this CPU or build cannot run); 2 a usage error.
//
// A pass does not say the hardware is sound. It says the environment can give a verdict. A
// failure does not say the hardware is faulty. It says the environment cannot be trusted to
// give one: the machine may be faulty, the build or the tool may be broken, or the platform may
// be unsupported, and selftest does not distinguish between those.

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "worker/services.hpp"

namespace loadforge::cli {

/// The units each workload runs on each path.
inline constexpr std::uint64_t kSelftestUnits = 3;

/// The footprint a workload is given unless `--footprint` says otherwise: small, so that the
/// whole command takes seconds even in a debug build.
inline constexpr std::uint64_t kSelftestFootprintBytes = std::uint64_t{3} << 20U;

/// What the command was asked for.
///
///   isa              the path to check (`--isa`); none: the best this CPU and each
///                    workload offer. Scalar is checked as well whenever it is not the path.
///   footprint_bytes  `--footprint`, an absolute size.
///   seed             `--seed`.
///   verbose          `--verbose`: the machine's and the build's paths, each check's tally,
///                    and every finding in full.
struct SelftestOptions {
  std::optional<simd::IsaPath> isa;
  std::uint64_t footprint_bytes = kSelftestFootprintBytes;
  std::uint64_t seed = 1;
  bool verbose = false;
};

/// The arguments after `selftest`: `--isa PATH`, `--footprint SIZE`, `--seed N` and `--verbose`,
/// in any order, each once. A refusal names the argument.
[[nodiscard]] core::Result<SelftestOptions, std::string> parse_selftest_args(
    std::span<const std::string_view> args);

/// The paths one workload is checked on, in order: the one this CPU would use for it
/// (`controller::choose_isa`: the requested path, or the widest the build, the CPU and the
/// workload all have), then scalar when that is another. A requested path the workload has no
/// kernel for cannot be checked on this workload: it is checked on scalar alone, and `note`
/// says why.
struct SelftestPaths {
  std::vector<simd::IsaPath> run;
  std::string note;
};

[[nodiscard]] SelftestPaths selftest_paths(std::optional<simd::IsaPath> requested,
                                           simd::IsaSet supported, simd::IsaSet compiled,
                                           simd::IsaSet workload_paths);

/// The digests the golden part compares with: main/selftest.hpp's, or a test's.
struct ExpectedDigests {
  std::uint64_t corpus = 0;
  std::uint64_t full = 0;
};

/// The checks, over the services given (the real ones in `selftest_command`; a test names
/// doubles), written to `out`. An `--isa` this build or CPU cannot run is refused on `err`,
/// before any check, with 1. Otherwise 0 when every check passed and kExitSelftestFailed when
/// any did not.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] int run_selftest(platform::Syscalls& syscalls, const worker::Services& services,
                               const SelftestOptions& options, const ExpectedDigests& expected,
                               std::ostream& out, std::ostream& err);

/// The command: its arguments after `selftest`, the real system calls and services, and this
/// source tree's expected digests.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] int selftest_command(std::span<const std::string_view> args, std::ostream& out,
                                   std::ostream& err);

}  // namespace loadforge::cli

#endif
