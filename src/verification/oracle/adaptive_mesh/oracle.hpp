// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_ADAPTIVE_MESH_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_ADAPTIVE_MESH_ORACLE_HPP

// The adaptive mesh workload's independent oracle (verification.md §3): the
// unit's output, as workloads/adaptive_mesh/spec.hpp lays it out, judged
// against the hotspots in the input by building the same mesh with a plain
// recursion, counting the leaves of every coarse cell, and summing the leaves
// of the sampled coarse cells in the one fixed order the specification names
// (oracle.hpp). The values of the leaves, the tolerance and its derivation
// are the specification's own helpers; the algorithm is a recursion, where the
// workload claims leaves from a shared queue.
//
// WHAT IT ANSWERS, IN ORDER
// -------------------------
//   1. evidence that is not kInputBytes in and kOutputBytes out: NotChecked.
//   2. a hotspot coordinate not below kLattice: NotChecked.
//   3. the leaf counts of coarse cells 0 .. kCoarseCells - 1, in order, then
//      the integrals of the kSampledCells sampled cells, in order of sample.
//      The first item that fails is a BoundedMismatch and ends the check;
//      blocks_compared counts the items evaluated, the failing one included.
//      All items right: an Agreement of kCoarseCells + kSampledCells.
//
// WHICH CELL SAMPLE j IS
// ----------------------
// A cell with an index in [8 j, 8 j + 8): sampled_cell() picks it by a mix of
// the unit index and j, so a re-run checks the same cells and different units
// check different ones.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/adaptive_mesh/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::adaptive_mesh {

/// The name of the workload this oracle checks.
inline constexpr std::string_view kWorkload = "adaptive_mesh";

/// The coarse cell sample `j` of unit `index` checks, j below kSampledCells:
/// an index in [8 j, 8 j + 8).
[[nodiscard]] std::size_t sampled_cell(std::uint64_t index, std::size_t j) noexcept;

class AdaptiveMeshOracle final : public Oracle {
 public:
  // Defined out of line, every one: an inline member used from several
  // translation units leaves one copy's counters at zero (journal §1.22).
  AdaptiveMeshOracle() noexcept;

  [[nodiscard]] std::string_view workload() const noexcept override;
  [[nodiscard]] Contract contract() const noexcept override;
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override;
};

/// The oracle for units of `plan`, the plan the adaptive mesh workload
/// resolved from `config`; kConfiguration when the two do not describe an
/// adaptive mesh unit: input or output sizes other than the specification's,
/// a resolved decomposition other than 1, or a configured one other than 0 (the
/// default) or 1. The registry's create() for "adaptive_mesh" calls this.
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

}  // namespace loadforge::verification::oracle::adaptive_mesh

#endif
