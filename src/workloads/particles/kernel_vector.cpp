// SPDX-License-Identifier: GPL-3.0-or-later
//
// The vector kernel of this build's native ISA path. Compiled with -mavx2 on
// x86-64 (src/workloads/particles/CMakeLists.txt); the unit holds nothing but
// the kernel, so no function of another unit's can be emitted here with AVX
// encodings (lane_kernel.hpp).

#include "workloads/particles/kernel.hpp"

#if defined(LOADFORGE_PARTICLES_AVX2) && !defined(__AVX2__)
#error "the AVX2 kernel is built with -mavx2"
#endif

#if defined(LOADFORGE_PARTICLES_AVX2) || defined(LOADFORGE_PARTICLES_NEON)
#include "workloads/particles/lane_kernel.hpp"
#endif

namespace loadforge::workloads::particles {

#if defined(LOADFORGE_PARTICLES_AVX2)
void cell_kernel_vector(const CellJob& job) noexcept {
  lane_cell_kernel<Lanes4>(job);
}
#elif defined(LOADFORGE_PARTICLES_NEON)
void cell_kernel_vector(const CellJob& job) noexcept {
  lane_cell_kernel<Lanes2>(job);
}
#endif

}  // namespace loadforge::workloads::particles
