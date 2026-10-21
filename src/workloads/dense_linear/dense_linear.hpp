// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_DENSE_LINEAR_DENSE_LINEAR_HPP
#define LOADFORGE_WORKLOADS_DENSE_LINEAR_DENSE_LINEAR_HPP

// The dense-linear workload (PLAN F14, IMPLEMENTATION M2b): C = A B for the
// n x n matrices spec.hpp defines, blocked by k-panels, parallel over row
// tiles through the executor, with one kernel per ISA path behind the dispatch
// seam (panel.hpp, simd/dispatch.hpp).
//
// THE SHAPE OF ONE UNIT
// ---------------------
// compute() runs kTiles tasks on the executor, task t owning rows
// [t * kTileRows, (t + 1) * kTileRows) of C. A task zeroes its rows and then
// applies the panels k0 = 0, kc, 2 kc, ... in that order, each by one call of
// the configured kernel. Every entry of C is therefore
//
//     c_ij = (((0 + p_ij(0)) + p_ij(1)) + ...) + p_ij(n/kc - 1)
//
// with each panel sum p_ij in the kernel's order. Nothing in it depends on the
// task's slot, on which thread runs it, on the order tasks run in, or on how
// many run at once: tasks share no output and the workload keeps no scratch.
// The panel depth kc fixes where the chains are cut, and the ISA path whether
// each step is fused, so the class is BitExactFixedDecomposition: the same
// bits for one (kc, path) at any thread count, and only within the bound of
// spec.hpp across either.
//
// FAILURE
// -------
// create() refuses, as kConfiguration, a panel depth spec.hpp does not list,
// an ISA path the kernel table has no kernel for, and a table without its
// scalar kernel; a footprint that holds no unit is kFootprintTooSmall. Once
// created the workload cannot fail: compute() is arithmetic on whatever bits
// it is given, and no input bit can change which memory it touches or how
// much work it does.

#include <cstdint>
#include <memory>
#include <string_view>

#include "core/result.hpp"
#include "primitives/simd/dispatch.hpp"
#include "workloads/dense_linear/panel.hpp"
#include "workloads/dense_linear/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::dense_linear {

using Kernels = simd::KernelTable<PanelKernel>;

/// The kernels this build holds: scalar always; AVX2 when the x86 kernels were
/// built, NEON when the ARM kernels were (neither in a LOADFORGE_SIMD=scalar
/// build). No AVX-512 kernel exists, so that path is always refused.
[[nodiscard]] const Kernels& built_kernels() noexcept;

/// What every dense_linear workload of this build says about itself, without
/// creating one (Registry::descriptor): the descriptor create() gives.
[[nodiscard]] Descriptor workload_descriptor();

/// The descriptor of a workload created with `kernels`: its paths are the
/// table's.
[[nodiscard]] Descriptor workload_descriptor(const Kernels& kernels);

/// The workload for `config`, with the kernels this build holds.
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

/// The workload for `config`, with `kernels`: the seam the tests use to put a
/// table in front of the workload that this build could not produce.
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config, const Kernels& kernels);

/// Bytes the algorithm itself moves for one unit (Plan::logical_bytes): A and
/// B read once and C written once, 3 n^2 doubles.
inline constexpr std::uint64_t kLogicalBytes = 3ULL * kOrder * kOrder * sizeof(double);

/// Floating-point operations in one unit (Plan::operations): n^3 multiplies and
/// n^3 additions, an FMA counted as the two it replaces.
inline constexpr std::uint64_t kOperations = 2ULL * kOrder * kOrder * kOrder;

}  // namespace loadforge::workloads::dense_linear

#endif
