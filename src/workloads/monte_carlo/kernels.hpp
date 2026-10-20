// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_MONTE_CARLO_KERNELS_HPP
#define LOADFORGE_WORKLOADS_MONTE_CARLO_KERNELS_HPP

// The chunk kernels of the Monte Carlo workload: one per ISA path, behind the
// dispatch seam (primitives/simd/dispatch.hpp).
//
// A kernel sums the samples k in [first, first + length) of one batch, in the
// order its path defines (spec.hpp, DECOMPOSITION). A path with L lanes keeps
// L accumulators, all starting at +0.0; sample k goes to accumulator
// (k - first) mod L, in increasing k; the accumulators are then added
// 0, 1, ..., L - 1 from accumulator 0. The scalar path has one lane, so its
// order is the plain left-to-right sum. Lanes per path: scalar 1, AVX2 4,
// NEON 2. AVX-512 has no kernel (the journal fragment says why).
//
// Preconditions: `first` and `length` are multiples of 2 L, length at least
// 2 L (every chunk length in spec.hpp's kChunkLengths is a multiple of 64).
// The workload keeps them; a kernel is entitled to misbehave otherwise.

#include <cstdint>

#include "primitives/simd/dispatch.hpp"

namespace loadforge::workloads::monte_carlo {

using ChunkKernel = double (*)(std::uint64_t key, std::uint64_t first,
                               std::uint32_t length) noexcept;

/// The kernels this build has, one slot per path; empty slots are paths it
/// has none for.
[[nodiscard]] simd::KernelTable<ChunkKernel> chunk_kernels() noexcept;

}  // namespace loadforge::workloads::monte_carlo

#endif
