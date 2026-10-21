// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/particles/kernels.hpp"

namespace loadforge::workloads::particles {

const simd::KernelTable<CellKernel>& cell_kernels() noexcept {
  static const simd::KernelTable<CellKernel> table = [] {
    simd::KernelTable<CellKernel> built;
    built.scalar = &cell_kernel_scalar;
#if defined(LOADFORGE_PARTICLES_AVX2)
    built.avx2 = &cell_kernel_vector;
#elif defined(LOADFORGE_PARTICLES_NEON)
    built.neon = &cell_kernel_vector;
#endif
    return built;
  }();
  return table;
}

}  // namespace loadforge::workloads::particles
