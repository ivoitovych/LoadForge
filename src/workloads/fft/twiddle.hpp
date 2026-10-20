// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_FFT_TWIDDLE_HPP
#define LOADFORGE_WORKLOADS_FFT_TWIDDLE_HPP

// The twiddle table W[m], m in [0, kPoints), of spec.hpp, built at compile
// time from the committed first octant (twiddle_table.hpp). Every other entry
// is an exact image of an octant entry -- a swap of parts and a sign change --
// so no arithmetic, and no libm, stands between the committed bits and the
// table the kernels read.

#include <array>
#include <bit>
#include <cstddef>
#include <span>

#include "workloads/fft/spec.hpp"
#include "workloads/fft/twiddle_table.hpp"

namespace loadforge::workloads::fft {

struct Complex {
  double re = 0.0;
  double im = 0.0;
};

namespace detail {

inline constexpr std::size_t kQuarter = kPoints / 4;
inline constexpr std::size_t kEighth = kPoints / 8;

/// An exact zero is +0.0, never -0.0 (spec.hpp).
[[nodiscard]] constexpr double plus_zero(double value) noexcept {
  return value == 0.0 ? 0.0 : value;
}

}  // namespace detail

/// W[m] = (RN(cos(2 pi m / n)), RN(-sin(2 pi m / n))) for m in [0, kPoints).
/// Precondition: m < kPoints.
[[nodiscard]] constexpr Complex twiddle(std::size_t m) noexcept {
  const std::size_t quadrant = m / detail::kQuarter;
  const std::size_t r = m % detail::kQuarter;
  // cos and sin of the angle 2 pi r / n, r in [0, n/4): the first octant
  // directly, the second by the mirror about pi/4.
  const bool mirrored = r > detail::kEighth;
  const std::span<const OctantRow> octant{kOctant};
  const OctantRow& row = octant[mirrored ? detail::kQuarter - r : r];
  const double first = std::bit_cast<double>(row.cos_bits);
  const double second = std::bit_cast<double>(row.sin_bits);
  const double c = mirrored ? second : first;
  const double s = mirrored ? first : second;
  // Rotating by `quadrant` quarter turns: (c, s) -> (cos, sin) of the angle.
  double cosine = c;
  double sine = s;
  if (quadrant == 1) {
    cosine = -s;
    sine = c;
  } else if (quadrant == 2) {
    cosine = -c;
    sine = -s;
  } else if (quadrant == 3) {
    cosine = s;
    sine = -c;
  }
  return Complex{detail::plus_zero(cosine), detail::plus_zero(-sine)};
}

namespace detail {

[[nodiscard]] constexpr std::array<Complex, kPoints> build_table() noexcept {
  std::array<Complex, kPoints> table{};
  const std::span<Complex> entries{table};
  for (std::size_t m = 0; m < kPoints; ++m) {
    entries[m] = twiddle(m);
  }
  return table;
}

}  // namespace detail

inline constexpr std::array<Complex, kPoints> kTwiddles = detail::build_table();

}  // namespace loadforge::workloads::fft

#endif
