// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_PLAN_HPP
#define LOADFORGE_CONTROLLER_PLAN_HPP

// What the controller decides before it forks, for each worker that runs a
// real workload: which workload, with what footprint, on which CPUs with how
// many threads, on which ISA path, at which duty cycle -- every choice made
// once, here, and handed to the worker as arguments, so that the workers, the
// re-runs and the report all argue from the same numbers.
//
// A worker whose configured name is not a registered workload is the null
// workload of M1 (the name is then a label, as it was), and the plan says so.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "core/result.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "primitives/simd/dispatch.hpp"
#include "topology/snapshot.hpp"
#include "verification/record.hpp"
#include "worker/entry.hpp"
#include "workloads/workload.hpp"

namespace loadforge::controller {

/// What the controller needs to run workloads that verify themselves.
///
///   registry      the workloads this build has; null: every configured name
///                 is a label for the null workload (M1).
///   seed          the run's seed; worker `slot` runs seed + slot * kSeedStride
///                 (each worker its own units, each reproducible from the
///                 provenance of its records alone).
///   footprint_bytes   a worker's footprint when its configuration names none.
///   decomposition     the decomposition parameter, 0 for each workload's default.
///   requested_isa     the path the user asked for, or none for the best the
///                     machine and workload offer.
///   isa_probe         what this CPU can run; null: scalar only.
///   duty              the duty cycle.
///   line_bytes        the topology's cache-line size; 0: none is known and no
///                     workload is started (a classification by an assumed
///                     line would be a fabricated fact, PLAN §4.5).
///   cpus              the CPUs workers may be placed on, ascending.
struct VerificationOptions {
  const workloads::Registry* registry = nullptr;
  std::uint64_t seed = 1;
  std::uint64_t footprint_bytes = std::uint64_t{32} << 20U;
  std::uint32_t decomposition = 0;
  std::optional<simd::IsaPath> requested_isa;
  const simd::IsaProbe* isa_probe = nullptr;
  verification::DutyCycle duty;
  std::uint32_t line_bytes = 0;
  std::vector<std::uint32_t> cpus;

  [[nodiscard]] friend bool operator==(const VerificationOptions&,
                                       const VerificationOptions&) = default;
};

/// The distance between two workers' seeds: an odd 64-bit constant, so that no
/// two slots share a seed and nearby slots do not share a stream's low bits.
inline constexpr std::uint64_t kSeedStride = 0x9E3779B97F4A7C15ULL;

/// One worker's plan.
///
///   args         what to tell the worker; none: it runs the null workload.
///   threads      how many threads its executor has.
///   cpus         the CPUs it is placed on (one per thread, cycling), or none.
///   description  one clause for the banner.
struct WorkerPlan {
  std::optional<worker::WorkloadArgs> args;
  std::uint32_t threads = 0;
  std::vector<std::uint32_t> cpus;
  std::string description;
  /// What the report says the worker ran; set with args.
  std::optional<WorkerWork> work;
};

/// `threads` CPUs for worker `slot` out of `all`: a block starting at
/// slot * threads, wrapping, without repeating a CPU (so at most all.size()).
/// Empty when `all` is empty.
[[nodiscard]] std::vector<std::uint32_t> assign_cpus(std::size_t slot, std::size_t threads,
                                                     const std::vector<std::uint32_t>& all);

/// The path a workload runs on: `requested` if the build, the machine and the
/// workload all have it, else the refusal; with no request, the first path in
/// simd::kPreference that all three have, scalar at worst. The workload's own
/// kernel set is a third set because select_path takes two: an unrequested
/// call would otherwise choose AVX-512 for a workload that has no such kernel
/// (journal fragment 06, question 2).
[[nodiscard]] core::Result<simd::IsaPath, std::string> choose_isa(
    std::optional<simd::IsaPath> requested, simd::IsaSet supported, simd::IsaSet compiled,
    simd::IsaSet workload_paths);

/// The plan for configured workload `entry` at worker `slot`, or why the run
/// cannot start: the workload refused its configuration, no ISA path will do,
/// no cache-line size is known. `budget` resolves a configured memory share.
[[nodiscard]] core::Result<WorkerPlan, std::string> plan_worker(
    const config::Workload& entry, std::size_t slot,
    const std::optional<config::MemoryBudget>& budget, const VerificationOptions& options,
    const topology::Snapshot* machine);

/// The floating-point environment as the banner states it.
[[nodiscard]] std::string describe_state(const fpenv::FpState& state);

}  // namespace loadforge::controller

#endif
