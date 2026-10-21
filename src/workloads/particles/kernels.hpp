// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_PARTICLES_KERNELS_HPP
#define LOADFORGE_WORKLOADS_PARTICLES_KERNELS_HPP

// The kernels this build has, one per ISA path (primitives/simd/dispatch.hpp).

#include "primitives/simd/dispatch.hpp"
#include "workloads/particles/kernel.hpp"

namespace loadforge::workloads::particles {

/// The table of this build: scalar always, and the native path unless the
/// build is LOADFORGE_SIMD=scalar.
[[nodiscard]] const simd::KernelTable<CellKernel>& cell_kernels() noexcept;

}  // namespace loadforge::workloads::particles

#endif
