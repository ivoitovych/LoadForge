// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_WORKLOAD_HPP
#define LOADFORGE_WORKLOADS_WORKLOAD_HPP

// CONTRACT (ADR 0004). What a workload is, to everything that runs, checks or
// reports one. Frozen: changed only by a contract change, never inside a
// stream.
//
//   Providers of Workload: one stream per workload (src/workloads/<name>/).
//   Provider of Registry: the integration stream that links them all.
//   Consumers: the verified runner (verification/runner.hpp), which runs a
//     workload's units in slices inside the input-integrity bracket; the
//     independent oracle for the workload (verification/oracle.hpp), which
//     reads the same plan; fault isolation's re-runs; results; explore.
//
// A WORKLOAD IS A FAMILY OF INDEPENDENT UNITS
// -------------------------------------------
// Unit i has an input that is a pure function of (seed, i) -- generate() --
// and an output that is a pure function of that input and the configuration
// -- compute(). Units share nothing. Three things follow, and they are why
// the contract has this shape rather than a "run for N seconds" call:
//
//   * Truth is regenerable. Any unit's input can be rebuilt anywhere, at any
//     time, from two integers, which is what the input-integrity bracket
//     (verification.md §4a) and fault isolation's re-runs (§7) both need.
//   * Determinism is a property of one unit. Thread count, which unit runs
//     where, and in what order, cannot change a unit's output; a workload's
//     class (determinism.md §2) is about what happens INSIDE compute().
//   * The runner owns memory, placement, slicing and verification. A workload
//     allocates nothing, starts no thread, reads no clock and makes no system
//     call: it is computation, and its tests are about computation.
//
// The runner places every unit's input and output at an address aligned to
// the cache-line size (record.hpp, Provenance), so a workload may assume that
// alignment and no more.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "core/result.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/record.hpp"
#include "worker/executor.hpp"

namespace loadforge::workloads {

/// docs/determinism.md §2. Encoded as these numbers wherever stored.
///
///   kBitExact                    identical bits across thread counts, runs,
///                                decompositions, ISA paths and architectures.
///   kBitExactFixedDecomposition  identical bits across thread counts and runs
///                                for one decomposition and one ISA path.
///   kBoundedResidual             reproducible only within a derived tolerance.
enum class DeterminismClass : std::uint8_t {
  kBitExact = 0,
  kBitExactFixedDecomposition = 1,
  kBoundedResidual = 2,
};

/// The class's name exactly as determinism.md and run.json spell it.
[[nodiscard]] constexpr std::string_view describe(DeterminismClass determinism) noexcept {
  switch (determinism) {
    case DeterminismClass::kBitExact:
      return "BitExact";
    case DeterminismClass::kBitExactFixedDecomposition:
      return "BitExactFixedDecomposition";
    case DeterminismClass::kBoundedResidual:
      return "BoundedResidual";
  }
  return "unrecognised determinism class";
}

/// What a workload says about itself. Constant for the workload's lifetime.
///
///   name         lower-case letters, digits and '_', starting with a letter:
///                the name configuration and reports use ("compression").
///   version      at least 1, and incremented whenever the bits a given
///                configuration produces change -- input or output. Golden
///                vectors and error records name it.
///   determinism  the class the workload is held to by its T6 tests.
///   contract     how its oracle checks it. A kBitExact workload is checked
///                exactly, and a kBoundedResidual one is bounded: bits that
///                may differ between runs cannot be compared exactly.
///   paths        the ISA paths it has kernels for; always holds kScalar.
struct Descriptor {
  std::string name;
  std::uint32_t version = 1;
  DeterminismClass determinism = DeterminismClass::kBitExact;
  verification::Contract contract = verification::Contract::kExact;
  simd::IsaSet paths{simd::IsaPath::kScalar};
};

[[nodiscard]] inline bool operator==(const Descriptor& lhs, const Descriptor& rhs) noexcept {
  return std::tie(lhs.name, lhs.version, lhs.determinism, lhs.contract, lhs.paths) ==
         std::tie(rhs.name, rhs.version, rhs.determinism, rhs.contract, rhs.paths);
}

/// Whether a name keeps the rule above.
[[nodiscard]] constexpr bool is_valid_name(std::string_view name) noexcept {
  if (name.empty() || name.front() < 'a' || name.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(
      name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

/// How a workload is configured.
///
///   seed             the run's seed: unit i's input is a function of (seed, i)
///                    and nothing else (primitives/rng/philox.hpp, stream i).
///   footprint_bytes  the most the resident units' inputs and outputs may
///                    take together. Scratch is outside it, and so is what the
///                    runner adds: the working set it maps is the units that
///                    fit, plus one more input (the bracket's regeneration),
///                    plus plan().scratch_bytes for every thread of the
///                    executor, plus a line of slack and the control state.
///                    (Sort at one unit: 2.0 units on one thread, 3.2 on
///                    three. Corrected in the M2 contract corrections: this
///                    said the runner sizes the footprint to leave room for
///                    scratch, which it does not.)
///   decomposition    the workload's decomposition parameter (tile, block or
///                    panel size, as its specification defines it); 0 asks
///                    for the workload's default. Part of the determinism
///                    contract for kBitExactFixedDecomposition.
///   isa              the dispatch path compute() uses, chosen once by the
///                    worker (simd/dispatch.hpp).
struct WorkloadConfig {
  std::uint64_t seed = 0;
  std::uint64_t footprint_bytes = 0;
  std::uint32_t decomposition = 0;
  simd::IsaPath isa = simd::IsaPath::kScalar;
};

[[nodiscard]] inline bool operator==(const WorkloadConfig& lhs,
                                     const WorkloadConfig& rhs) noexcept {
  return std::tie(lhs.seed, lhs.footprint_bytes, lhs.decomposition, lhs.isa) ==
         std::tie(rhs.seed, rhs.footprint_bytes, rhs.decomposition, rhs.isa);
}

/// The geometry a configuration resolves to. Constant for the workload's
/// lifetime.
///
///   input_bytes, output_bytes  per unit; both at least 1.
///   scratch_bytes  per executor slot: compute() is given scratch_bytes times
///                  the executor's threads(), and slot s owns the s-th piece.
///                  May be 0.
///   units          how many units' inputs and outputs fit the footprint:
///                  at least 1, and units * (input_bytes + output_bytes) <=
///                  footprint_bytes.
///   decomposition  the decomposition in use: the configured one, or the
///                  default it resolved to. Never 0.
///   logical_bytes  the memory traffic one unit's computation performs by its
///                  algorithm (PLAN F12, logical bandwidth): known without
///                  any counter, so the memory column is never empty.
///   operations     the arithmetic or comparison operations one unit
///                  performs, in the workload's own unit, for rates.
struct Plan {
  std::uint64_t input_bytes = 0;
  std::uint64_t output_bytes = 0;
  std::uint64_t scratch_bytes = 0;
  std::uint64_t units = 0;
  std::uint32_t decomposition = 0;
  std::uint64_t logical_bytes = 0;
  std::uint64_t operations = 0;
};

[[nodiscard]] inline bool operator==(const Plan& lhs, const Plan& rhs) noexcept {
  return std::tie(lhs.input_bytes, lhs.output_bytes, lhs.scratch_bytes, lhs.units,
                  lhs.decomposition, lhs.logical_bytes, lhs.operations) ==
         std::tie(rhs.input_bytes, rhs.output_bytes, rhs.scratch_bytes, rhs.units,
                  rhs.decomposition, rhs.logical_bytes, rhs.operations);
}

/// Why a workload could not be created.
///
///   kUnknownWorkload    no workload of that name (Registry only).
///   kConfiguration      the configuration is outside the workload's domain: a
///                       decomposition it does not accept, an ISA path it has
///                       no kernel for. The detail names the value.
///   kFootprintTooSmall  not even one unit fits the footprint.
enum class WorkloadFault : std::uint8_t { kUnknownWorkload, kConfiguration, kFootprintTooSmall };

[[nodiscard]] constexpr std::string_view describe(WorkloadFault fault) noexcept {
  switch (fault) {
    case WorkloadFault::kUnknownWorkload:
      return "unknown workload";
    case WorkloadFault::kConfiguration:
      return "configuration outside the workload's domain";
    case WorkloadFault::kFootprintTooSmall:
      return "footprint too small for one unit";
  }
  return "unrecognised workload fault";
}

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct WorkloadError {
  WorkloadFault fault = WorkloadFault::kConfiguration;
  std::string detail;
};

[[nodiscard]] inline bool operator==(const WorkloadError& lhs, const WorkloadError& rhs) noexcept {
  return std::tie(lhs.fault, lhs.detail) == std::tie(rhs.fault, rhs.detail);
}

/// One workload, configured. Created by its provider's factory, which takes a
/// WorkloadConfig and returns the workload or a WorkloadError; once created it
/// cannot fail.
///
/// generate(index, input)
///   Fills `input` -- exactly plan().input_bytes -- with unit `index`'s input:
///   a pure function of (config.seed, index). It does not depend on the
///   decomposition, the ISA path, previous calls or their order, or the
///   executor; two workloads created with the same seed and footprint
///   generate the same bytes. Distinct indices give distinct inputs.
///
/// compute(index, input, output, scratch, executor)
///   Writes unit `index`'s output into `output` -- exactly
///   plan().output_bytes -- from `input`, using `scratch` -- exactly
///   plan().scratch_bytes * executor.threads() -- and, if it parallelises the
///   unit, `executor`. Every byte of `output` is written: nothing left in it
///   from before survives. Neither what scratch held before nor the
///   executor's thread count changes the output for kBitExact and
///   kBitExactFixedDecomposition workloads.
///
///   compute() is TOTAL over input bytes: an input that is not what generate()
///   wrote -- because memory corrupted it, which is the event this tool
///   exists to see -- is computed on like any other, without a crash,
///   without reading or writing outside the spans, and without a different
///   amount of work escaping its bound. A NaN, a garbage length field, an
///   offset pointing nowhere: each is input, never undefined behaviour.
///
/// Preconditions of both: spans of exactly the stated sizes. A workload is
/// entitled to terminate when they are broken; that is a runner bug.
///
/// Thread-safety: generate() and compute() are const and may be called
/// concurrently, for distinct units, with distinct buffers and scratch. A
/// workload holds no mutable state.
class Workload {
 public:
  Workload() = default;
  Workload(const Workload&) = delete;
  Workload(Workload&&) = delete;
  Workload& operator=(const Workload&) = delete;
  Workload& operator=(Workload&&) = delete;
  virtual ~Workload() = default;

  [[nodiscard]] virtual const Descriptor& descriptor() const noexcept = 0;
  [[nodiscard]] virtual const Plan& plan() const noexcept = 0;
  virtual void generate(std::uint64_t index, std::span<std::byte> input) const noexcept = 0;
  virtual void compute(std::uint64_t index, std::span<const std::byte> input,
                       std::span<std::byte> output, std::span<std::byte> scratch,
                       worker::Executor& executor) const noexcept = 0;
};

/// Every workload a build contains, by name.
///
/// names() is sorted, without duplicates, every entry a valid name. create()
/// answers a workload whose descriptor().name is the name asked for, or the
/// workload's own error for a configuration it refuses, or kUnknownWorkload
/// for a name not in names().
///
/// descriptor(name) answers what the workload of that name says about itself
/// WITHOUT creating one: for every name in names() it equals
/// create(name, c).value()->descriptor() for every configuration c that
/// create() accepts, and it is the same on every call; kUnknownWorkload, with
/// the same words as create(), for a name not in names(). It exists because
/// the path a workload is created for must be chosen from the paths it has
/// (Descriptor::paths), and the workload cannot be created before its path is
/// chosen (contract change of the M2 corrections, 2026-10-21: before it, a
/// consumer made every workload twice, once on the scalar path to read this).
/// It takes no configuration because a descriptor is constant for the
/// workload's lifetime and does not depend on one.
///
/// Thread-safety: all three calls may be made from any thread.
class Registry {
 public:
  Registry() = default;
  Registry(const Registry&) = delete;
  Registry(Registry&&) = delete;
  Registry& operator=(const Registry&) = delete;
  Registry& operator=(Registry&&) = delete;
  virtual ~Registry() = default;

  [[nodiscard]] virtual std::vector<std::string> names() const = 0;
  [[nodiscard]] virtual core::Result<std::unique_ptr<Workload>, WorkloadError> create(
      std::string_view name, const WorkloadConfig& config) const = 0;
  [[nodiscard]] virtual core::Result<Descriptor, WorkloadError> descriptor(
      std::string_view name) const = 0;
};

}  // namespace loadforge::workloads

#endif
