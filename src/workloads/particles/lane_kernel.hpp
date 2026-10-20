// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_PARTICLES_LANE_KERNEL_HPP
#define LOADFORGE_WORKLOADS_PARTICLES_LANE_KERNEL_HPP

// The force sum over vector lanes, written once with the compilers' portable
// vector types and instantiated for the lane count of each ISA path: four
// doubles for AVX2, two for NEON (spec.hpp, DECOMPOSITION: the lane count is
// part of the decomposition, and with it the order the terms are added in).
//
// WHAT IS ADDED, AND IN WHAT ORDER. Each lane keeps its own accumulator for
// each component. Candidate j of a run goes to lane (j - run.begin) mod lanes
// of its vector; the runs are taken in order and a run's last vector is
// masked, so lanes past the run's end add exactly nothing. At the end of the
// particle the lanes are combined in a fixed shape: (l0 + l1) + (l2 + l3).
// That is a function of the cell size, the ISA path and the data, never of the
// thread count.
//
// WHY NOT INTRINSICS. Intrinsics may appear only under src/primitives/simd/
// (the portability workflow refuses them elsewhere), which this stream does
// not own. Vector types need none, build on both architectures, and the
// kernel is compiled for its path by the unit's own flags (-mavx2 on x86-64).
// No multiply-add is fused (-ffp-contract=off, and the vector types have no
// fused operator), which the specification's evaluation model allows.
//
// THIS FILE AND THE LINKER. The functions are in an unnamed namespace and call
// nothing from the standard library: in the AVX2 unit a standard inline
// function would be emitted with AVX encodings, and the linker could keep that
// copy for callers on a machine without them.

#include <cstddef>
#include <cstdint>

#include "workloads/particles/kernel.hpp"

// The kernels index raw pointers on purpose: a std::span's operator[] is a
// standard inline function (see above), and the runs keep every access inside
// the padded arrays.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)

namespace loadforge::workloads::particles {
namespace {

struct Lanes2 {
  using Real = double __attribute__((vector_size(16)));
  using Mask = decltype(Real{} <= Real{});
  static constexpr std::uint32_t kLanes = 2;
  static Mask index() noexcept { return Mask{0, 1}; }
  static double sum(Real v) noexcept { return v[0] + v[1]; }
};

struct Lanes4 {
  using Real = double __attribute__((vector_size(32)));
  using Mask = decltype(Real{} <= Real{});
  static constexpr std::uint32_t kLanes = 4;
  static Mask index() noexcept { return Mask{0, 1, 2, 3}; }
  static double sum(Real v) noexcept { return (v[0] + v[1]) + (v[2] + v[3]); }
};

template <typename Lanes>
void lane_cell_kernel(const CellJob& job) noexcept {
  using Real = typename Lanes::Real;
  using Mask = typename Lanes::Mask;
  constexpr std::uint32_t kLanes = Lanes::kLanes;
  const Real one = Real{} + 1.0;
  const Real quarter = Real{} + 0.25;
  const Real zero = Real{};
  const Mask index = Lanes::index();
  for (std::uint32_t i = job.first; i < job.last; ++i) {
    const Real xi = Real{} + job.x[i];
    const Real yi = Real{} + job.y[i];
    const Real zi = Real{} + job.z[i];
    Real ax = zero;
    Real ay = zero;
    Real az = zero;
    for (std::size_t r = 0; r < job.run_count; ++r) {
      const std::uint32_t end = job.runs[r].end;
      for (std::uint32_t j = job.runs[r].begin; j < end; j += kLanes) {
        Real xj;
        Real yj;
        Real zj;
        __builtin_memcpy(&xj, job.x + j, sizeof(Real));
        __builtin_memcpy(&yj, job.y + j, sizeof(Real));
        __builtin_memcpy(&zj, job.z + j, sizeof(Real));
        const Real dx = xi - xj;
        const Real dy = yi - yj;
        const Real dz = zi - zj;
        const Real q = dx * dx + dy * dy + dz * dz;
        const Real t = one - q * quarter;
        // A term is zero when t <= 0 and active otherwise, a NaN t included.
        Mask keep = ~(t <= zero);
        const std::uint32_t remaining = end - j;
        if (remaining < kLanes) {
          keep &= index < (Mask{} + static_cast<std::int64_t>(remaining));
        }
        const Real w = t * t;
        const Real tx = w * dx;
        const Real ty = w * dy;
        const Real tz = w * dz;
        ax += __builtin_bit_cast(Real, __builtin_bit_cast(Mask, tx) & keep);
        ay += __builtin_bit_cast(Real, __builtin_bit_cast(Mask, ty) & keep);
        az += __builtin_bit_cast(Real, __builtin_bit_cast(Mask, tz) & keep);
      }
    }
    job.force[3 * static_cast<std::size_t>(i)] = Lanes::sum(ax);
    job.force[3 * static_cast<std::size_t>(i) + 1] = Lanes::sum(ay);
    job.force[3 * static_cast<std::size_t>(i) + 2] = Lanes::sum(az);
  }
}

}  // namespace
}  // namespace loadforge::workloads::particles

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

#endif
