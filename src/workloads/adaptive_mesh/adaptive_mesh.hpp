// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_ADAPTIVE_MESH_ADAPTIVE_MESH_HPP
#define LOADFORGE_WORKLOADS_ADAPTIVE_MESH_ADAPTIVE_MESH_HPP

// The adaptive mesh workload (IMPLEMENTATION M4): a quadtree refined around a
// unit's hotspots, its leaves valued and gathered into 16 x 16 coarse cells,
// as workloads/adaptive_mesh/spec.hpp defines.
//
// THE SHAPE OF ONE UNIT
// ---------------------
// compute() first builds the mesh on the calling thread: integer logic over
// the 256 hotspots, writing the leaves of every coarse cell, in leaf order,
// to one table on the stack, and the leaf counts to the output. Then it hands
// the leaves to the executor, one task per leaf. A task values its leaf (a
// fixed sequence of binary64 operations, spec.hpp) and adds it to the partial
// sum of its slot for the leaf's coarse cell. When the executor returns, each
// coarse cell's integral is the sum of its slots' partial sums, in slot order.
//
// WHY BoundedResidual
// -------------------
// Which slot values a leaf depends on the schedule: a thread that finishes
// early takes the next leaf. So a cell's leaves are grouped into partial sums
// differently from run to run, and the integral differs in the last bits.
// Every grouping and order is within the bound spec.hpp derives, which is the
// class. Counts and leaf values are exact and identical on every run.
//
// FAILURE
// -------
// create() refuses, as kConfiguration, a decomposition other than 0 or 1 and
// an ISA path other than scalar (the arithmetic is a fixed sequence, so there
// is nothing for a vector path to do); a footprint that holds no unit is
// kFootprintTooSmall. Once created the workload cannot fail: whatever bytes
// the input holds, at most 256 hotspots exist, a hotspot splits at most five
// cells, and a hotspot outside the lattice splits none, so the work and the
// table are bounded by the input's size alone.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "core/result.hpp"
#include "workloads/adaptive_mesh/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::adaptive_mesh {

/// Descriptor::version: incremented when the bits a configuration produces change.
inline constexpr std::uint32_t kWorkloadVersion = 1;

/// The most leaves any input can make: the 256 coarse cells, and three more
/// for each of the at most kMaxLevel - kBaseLevel cells a hotspot splits.
inline constexpr std::size_t kMaxLeaves =
    kCoarseCells + (std::size_t{3} * (kMaxLevel - kBaseLevel) * kHotspots);

/// Scratch per executor slot (Plan::scratch_bytes): one partial sum per coarse cell.
inline constexpr std::uint64_t kScratchBytes = kCoarseCells * sizeof(double);

/// Bytes the algorithm moves for one unit (Plan::logical_bytes): the hotspots
/// read once and the integrals and counts written once.
inline constexpr std::uint64_t kLogicalBytes = kInputBytes + kOutputBytes;

/// Leaves in a generated unit, on average (measured, docs/journal/31-workload-adaptive-mesh.md).
inline constexpr std::uint64_t kTypicalLeaves = 3700;
/// Binary64 operations in one hotspot term: two subtractions, two squares, an
/// addition, a scaling, an addition and a division.
inline constexpr std::uint64_t kOperationsPerTerm = 8;
/// Plan::operations: a typical unit's terms and their summation, one add each.
inline constexpr std::uint64_t kOperations = kTypicalLeaves * kHotspots * (kOperationsPerTerm + 1);

/// The workload for `config`.
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

}  // namespace loadforge::workloads::adaptive_mesh

#endif
