// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_COMPRESSION_GENERATOR_HPP
#define LOADFORGE_WORKLOADS_COMPRESSION_GENERATOR_HPP

// The compression workload's input: data an LZ compressor can find matches in
// and cannot reduce to nothing (spec.hpp: it compresses to between a fifth and
// four fifths of its size, so literals and matches are both exercised).
//
// HOW
// ---
// The buffer starts as Philox bytes, stream `index`, which no compressor can
// shrink. A walk from the front then alternates two kinds of run, each drawn
// from the same stream at positions far above the bytes' own:
//
//   * a literal run of 1 to kMaxLiteralRun bytes, left as they are;
//   * a copy run of kMinCopyRun to kMaxCopyRun bytes, each byte copied from
//     `distance` bytes earlier, the distance drawn in [1, min(at, kMaxDistance)]
//     -- a distance shorter than the run repeats its own output, as the
//     format's overlapping matches do.
//
// The proportion of literal to copied bytes sets how far the data compresses;
// the T4 tests measure where the compressor lands over many units and seeds.
//
// Draw j of a unit is u64_of(block_at(seed, index, kControlPosition + j))
// (philox.hpp's convention for integer draws), and the bytes are its first
// size / 16 blocks: the two never overlap for any buffer below 2^66 bytes.
//
// DETERMINISM: a pure function of (seed, index) and the buffer's length,
// integer arithmetic only, unsigned throughout.

#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::workloads::compression {

inline constexpr std::uint64_t kControlPosition = std::uint64_t{1} << 62U;
inline constexpr std::size_t kMaxLiteralRun = 40;
inline constexpr std::size_t kMinCopyRun = 4;
inline constexpr std::size_t kMaxCopyRun = 64;
inline constexpr std::size_t kMaxDistance = 32768;

/// Fills `input` with unit `index`'s input under `seed`, as above.
void generate_input(std::uint64_t seed, std::uint64_t index, std::span<std::byte> input) noexcept;

}  // namespace loadforge::workloads::compression

#endif
