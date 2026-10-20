// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_MEMORY_MEMORY_HPP
#define LOADFORGE_PRIMITIVES_MEMORY_MEMORY_HPP

// The memory calibration load (PLAN §6, layer 1): streaming traffic. Each unit
// writes its own generated block of memory once, then copies it and reads the
// copy back, pass after pass. The buffer is the caller's, so the working set is
// the caller's choice and nothing here allocates.
//
// THE DEFINITION. Unit u's source buffer holds the first `bytes` bytes of
// rng::fill_stream(seed, u). A pass copies the source to the destination buffer
// and adds the destination's 64-bit little-endian words, wrapping, rotated left
// by the pass number mod 64, to the digest. The unit's digest is that sum over
// `passes` passes; with no passes, or no bytes, it is zero.
//
// Regenerating the stream is how the copy is checked (verification.md §4a): the
// source is a pure function of (seed, unit), so its expected content never has
// to be stored, and a test recomputes the digest from the definition.
//
// DETERMINISM: BitExact -- integers only, wrapping unsigned sums. The words are
// read as little-endian, which is the layout of every supported machine (x86-64
// and AArch64 Linux); the static_assert in memory.cpp refuses a big-endian one
// rather than let its digests differ from everyone else's.
// THREAD-SAFETY: as for the cache primitive: a running unit owns its slot's
// scratch exclusively.

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/result.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::primitives::memory {

inline constexpr workloads::DeterminismClass kDeterminism =
    workloads::DeterminismClass::kBitExact;

inline constexpr std::uint64_t kWordBytes = 8;

struct Config {
  std::uint64_t seed = 0;
  /// Bytes in each unit's buffers. A multiple of kWordBytes; zero is legal.
  std::uint64_t bytes = 0;
  std::uint64_t passes = 0;
};

enum class ConfigError : std::uint8_t {
  kBytesNotWholeWords,  ///< bytes is not a multiple of kWordBytes
  kScratchTooSmall,     ///< the scratch cannot hold a source and a destination per slot
};

/// 64-bit words of scratch one running unit needs: a source and a destination.
[[nodiscard]] core::Result<std::uint64_t, ConfigError> scratch_words(
    const Config& config) noexcept;

/// The digest of one unit, using `scratch` (at least scratch_words(config)
/// words). Precondition: the configuration is valid and the scratch large enough.
[[nodiscard]] std::uint64_t unit_digest(const Config& config, std::uint64_t unit,
                                        std::span<std::uint64_t> scratch) noexcept;

/// Runs partials.size() units, each in its slot's share of `scratch`
/// (executor.threads() shares of scratch_words(config) words), writing unit i's
/// digest to partials[i], and returns their wrapping sum in index order.
[[nodiscard]] core::Result<std::uint64_t, ConfigError> run(worker::Executor& executor,
                                                           const Config& config,
                                                           std::span<std::uint64_t> scratch,
                                                           std::span<std::uint64_t> partials) noexcept;

}  // namespace loadforge::primitives::memory

#endif
