// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_FLOATING_FLOATING_HPP
#define LOADFORGE_PRIMITIVES_FLOATING_FLOATING_HPP

// The floating-point calibration load (PLAN §6, layer 1): a dot product of two
// generated vectors, accumulated by fused multiply-add into eight interleaved
// accumulators. Not user-facing.
//
// THE DEFINITION. Element j of unit u is one Philox block b = block_at(seed, u,
// j); x = unit_interval(b[0] | b[1] << 32) and y = unit_interval(b[2] | b[3] <<
// 32). Accumulator l (l = j mod 8) takes acc[l] = fma(x, y, acc[l]). The unit's
// value reduces the accumulators in the fixed tree
//   ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7)).
// The run's value adds the units' values left to right, in index order.
//
// DETERMINISM: BitExactFixedDecomposition. The sum rounds, so its bits depend on
// the decomposition, and the decomposition is fixed here: eight accumulators,
// the tree above, units added in index order. Nothing depends on the thread
// count or on completion order (determinism.md §3). The eight accumulators are
// a decomposition like a SIMD width is, which is why the class is not BitExact.
// std::fma is correctly rounded on every IEEE 754 machine, so the bits agree
// across x86-64 and ARM64 on this scalar path; whether it runs as one
// instruction or a library call is the toolchain's business. A stress kernel
// that must name the instruction belongs to the simd stream, not to this
// calibration load.
// THREAD-SAFETY: as for the integer primitive.

#include <cstddef>
#include <cstdint>
#include <span>

#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::primitives::floating {

inline constexpr workloads::DeterminismClass kDeterminism =
    workloads::DeterminismClass::kBitExactFixedDecomposition;

inline constexpr std::size_t kAccumulators = 8;
/// The most elements per unit for which the exact integer oracle (a sum of
/// 106-bit products in 128 bits) cannot overflow. The kernel itself has no limit.
inline constexpr std::uint64_t kMaxExactElements = std::uint64_t{1} << 20U;

struct Config {
  std::uint64_t seed = 0;
  /// Elements per unit. Zero is legal and gives +0.0.
  std::uint64_t elements = 0;
};

/// The value of one unit, a pure function of its arguments.
[[nodiscard]] double unit_value(const Config& config, std::uint64_t unit) noexcept;

/// Runs partials.size() units, writing unit i's value to partials[i], and
/// returns their sum in index order (+0.0 for none).
[[nodiscard]] double run(worker::Executor& executor, const Config& config,
                         std::span<double> partials) noexcept;

}  // namespace loadforge::primitives::floating

#endif
