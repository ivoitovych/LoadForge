// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <cstdint>

#include "workloads/particles/kernels.hpp"

// Raw pointers, as in lane_kernel.hpp, so the two kernels are read side by side.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)

namespace loadforge::workloads::particles {

void cell_kernel_scalar(const CellJob& job) noexcept {
  for (std::uint32_t i = job.first; i < job.last; ++i) {
    const double xi = job.x[i];
    const double yi = job.y[i];
    const double zi = job.z[i];
    double ax = 0.0;
    double ay = 0.0;
    double az = 0.0;
    for (std::size_t r = 0; r < job.run_count; ++r) {
      for (std::uint32_t j = job.runs[r].begin; j < job.runs[r].end; ++j) {
        const double dx = xi - job.x[j];
        const double dy = yi - job.y[j];
        const double dz = zi - job.z[j];
        const double q = dx * dx + dy * dy + dz * dz;
        const double t = 1.0 - q * 0.25;
        // Zero when t <= 0; active otherwise, a NaN t included.
        if (t <= 0.0) {
          continue;
        }
        const double w = t * t;
        ax += w * dx;
        ay += w * dy;
        az += w * dz;
      }
    }
    job.force[3 * static_cast<std::size_t>(i)] = ax;
    job.force[3 * static_cast<std::size_t>(i) + 1] = ay;
    job.force[3 * static_cast<std::size_t>(i) + 2] = az;
  }
}

}  // namespace loadforge::workloads::particles

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
