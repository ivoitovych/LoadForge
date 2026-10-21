// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_PARTICLES_KERNEL_HPP
#define LOADFORGE_WORKLOADS_PARTICLES_KERNEL_HPP

// The seam between the neighbour search and the force sum.
//
// The search (workload.cpp) bins the particles into cells, sorts them by cell
// and, for each cell, lists the runs of sorted particles that lie in the cells
// within the cutoff. A kernel sums, for every particle of the cell, the terms
// of those runs (spec.hpp, "The evaluation model") and writes the force at the
// particle's sorted position.
//
// This header is included by translation units compiled with -mavx2 (the
// vector kernel), so it declares types and nothing else: a function defined
// here would be emitted in that unit too, and the linker may keep that copy
// for callers on a machine without AVX2.

#include <cstddef>
#include <cstdint>

// Which vector kernel this build has, from the same facts as
// simd::compiled_paths() (primitives/simd/dispatch.hpp).
#if defined(LOADFORGE_SIMD_SCALAR)
#elif defined(__x86_64__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): selects which kernel is compiled
#define LOADFORGE_PARTICLES_AVX2 1
#elif defined(__aarch64__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): selects which kernel is compiled
#define LOADFORGE_PARTICLES_NEON 1
#endif

namespace loadforge::workloads::particles {

/// The most lanes any kernel reads at once. The sorted coordinate arrays carry
/// this many zeros past the last particle, so a vector load that starts at the
/// last particle stays inside them.
inline constexpr std::size_t kMaxLanes = 4;

/// A half-open range of sorted particles: all of them lie in cells within the
/// cutoff of the cell being summed.
struct NeighbourRun {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
};

/// The forces on the particles [first, last) of one cell.
///
///   x, y, z   the sorted coordinates, n + kMaxLanes entries each
///   runs      run_count ranges, in the order the terms are visited
///   force     3 doubles per sorted position: force[3 k + c] is component c
///             of the force on sorted particle k
struct CellJob {
  const double* x = nullptr;
  const double* y = nullptr;
  const double* z = nullptr;
  const NeighbourRun* runs = nullptr;
  std::size_t run_count = 0;
  std::uint32_t first = 0;
  std::uint32_t last = 0;
  double* force = nullptr;
};

using CellKernel = void (*)(const CellJob& job) noexcept;

/// The portable reference: one accumulator per component, the terms added in
/// the order of the runs. Bit-identical on x86-64 and ARM64.
void cell_kernel_scalar(const CellJob& job) noexcept;

#if defined(LOADFORGE_PARTICLES_AVX2) || defined(LOADFORGE_PARTICLES_NEON)
/// The native vector path: four lanes (AVX2) or two (NEON).
void cell_kernel_vector(const CellJob& job) noexcept;
#endif

}  // namespace loadforge::workloads::particles

#endif
