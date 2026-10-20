// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_INTEGER_INTEGER_HPP
#define LOADFORGE_PRIMITIVES_INTEGER_INTEGER_HPP

// The integer calibration load (PLAN §6, layer 1): four independent 64-bit
// linear congruential chains per unit, advanced in lock step. It keeps the
// integer multiplier and adder busy and is deliberately not a user-facing test.
//
// THE DEFINITION. Lane k of unit u starts at x0 = u64_of(block_at(seed, u, k))
// and takes `steps` steps of x -> A*x + C modulo 2^64 (Knuth's MMIX constants).
// The unit's digest folds the four final values: f0 ^ rotl(f1,16) ^ rotl(f2,32)
// ^ rotl(f3,48). The run's digest is the wrapping sum of the units' digests.
//
// DETERMINISM: BitExact. Unsigned modular arithmetic throughout (determinism.md
// §2: no signed overflow), so the unit digest is a pure function of
// (seed, unit, steps) and the sum of per-unit digests does not depend on the
// order, the thread count or the architecture. The run still adds them in
// index order, the one order that is defined.
// THREAD-SAFETY: every function is safe to call concurrently; run() follows the
// Executor's rule of one caller at a time per executor.

#include <cstddef>
#include <cstdint>
#include <span>

#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::primitives::integer {

inline constexpr workloads::DeterminismClass kDeterminism = workloads::DeterminismClass::kBitExact;

/// Lanes advanced together in each unit.
inline constexpr std::size_t kLanes = 4;
/// The step: x -> kMultiplier * x + kIncrement, modulo 2^64.
inline constexpr std::uint64_t kMultiplier = 6364136223846793005ULL;
inline constexpr std::uint64_t kIncrement = 1442695040888963407ULL;

struct Config {
  std::uint64_t seed = 0;
  /// Steps each lane takes. Zero is legal: the digest is then of the start.
  std::uint64_t steps = 0;
};

/// The digest of one unit. A pure function of its arguments.
[[nodiscard]] std::uint64_t unit_digest(const Config& config, std::uint64_t unit) noexcept;

/// Runs partials.size() units over the executor, writing unit i's digest to
/// partials[i], and returns their wrapping sum, taken in index order. An empty
/// span runs nothing and returns zero.
[[nodiscard]] std::uint64_t run(worker::Executor& executor, const Config& config,
                                std::span<std::uint64_t> partials) noexcept;

}  // namespace loadforge::primitives::integer

#endif
