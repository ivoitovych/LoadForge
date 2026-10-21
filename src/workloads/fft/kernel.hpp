// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_FFT_KERNEL_HPP
#define LOADFORGE_WORKLOADS_FFT_KERNEL_HPP

// The transform: t stages of radix r, each stage a Stockham autosort pass in
// decimation-in-time form, and the kernels that run one stage's butterflies.
//
// A stage combines the r sub-transforms of length L = r^k into transforms of
// length r L. With n the transform length and M = n / (r L), butterfly
// (v, c), v in [0, L), c in [0, M), reads the r elements at complex index
//   v r M + c + a M,   a in [0, r),
// multiplies the a-th by W[a v M s] (s = kPoints / n; the first by nothing),
// forms the r-point DFT of the products, and writes output b at complex index
//   (v + b L) M + c.
// Every element of a stage's input is read by exactly one butterfly, and
// every element of its output written by exactly one, so the butterflies of a
// stage are independent and any split of them over threads gives the same
// bits. The data is natural-order on the way in and on the way out: no
// digit-reversal pass exists.
//
// Elements are 16-byte records (re, im), each part a little-endian IEEE 754
// double, read and written byte by byte so that neither alignment nor the
// machine's byte order enters.

#include <cstddef>
#include <cstdint>
#include <span>

#include "primitives/simd/dispatch.hpp"
#include "worker/executor.hpp"

namespace loadforge::workloads::fft {

/// Bytes of one complex element.
inline constexpr std::size_t kElementBytes = 16;

/// What a stage kernel needs to know about its stage.
///   length   L, the length of the sub-transforms the stage's input holds.
///   tail     M, the number of interleaved sub-transforms: n / (r L).
///   stride   kPoints / n: the step in the twiddle table for transform length n.
struct StageGeometry {
  std::size_t length = 1;
  std::size_t tail = 1;
  std::size_t stride = 1;
};

/// Runs butterflies [first, last) of one stage, numbered v M + c.
using StageKernel = void (*)(const StageGeometry& geometry, std::span<const std::byte> in,
                             std::span<std::byte> out, std::size_t first,
                             std::size_t last) noexcept;

/// The stage kernels of a radix, one per ISA path. Precondition: is_radix().
[[nodiscard]] simd::KernelTable<StageKernel> stage_kernels(std::uint32_t radix) noexcept;

/// Butterflies one task of a stage covers. A constant, not a function of the
/// executor's thread count, so how a stage is split never depends on it.
inline constexpr std::size_t kButterfliesPerTask = 256;

/// Transforms the n-point vector in `in` into `out`, forward and unnormalised.
///
/// Preconditions: n is a power of `radix` (2 or 4), at least `radix`, at most
/// kPoints; in, out and scratch each hold n * kElementBytes bytes and do not
/// overlap. `scratch` is the one buffer the stages ping-pong with `out`.
void transform(StageKernel kernel, std::uint32_t radix, std::size_t n,
               std::span<const std::byte> in, std::span<std::byte> out,
               std::span<std::byte> scratch, worker::Executor& executor) noexcept;

/// Little-endian IEEE 754 double at element `index` (in doubles) of `bytes`.
[[nodiscard]] double load_double(std::span<const std::byte> bytes, std::size_t index) noexcept;
void store_double(std::span<std::byte> bytes, std::size_t index, double value) noexcept;

}  // namespace loadforge::workloads::fft

#endif
