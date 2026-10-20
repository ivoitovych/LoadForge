// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_RNG_PHILOX_HPP
#define LOADFORGE_PRIMITIVES_RNG_PHILOX_HPP

// CONTRACT (ADR 0004). The counter-based generator every workload, oracle and
// fault-isolation re-run draws its data from (docs/determinism.md §3, PLAN F8,
// F18). Frozen: changed only by a contract change, never inside a stream.
//
// WHY THIS CONTRACT CARRIES ITS DEFINITION
// ----------------------------------------
// Most contracts here are interfaces, and their implementations belong to the
// streams that provide them. This one is defined in full, in the header, for
// three reasons recorded in ADR 0004: there is exactly one correct reading of
// it -- Philox4x32-10 as published by Salmon et al. (SC'11) and pinned by the
// Random123 known-answer vectors -- so a second, independent implementation
// would add no second reading, only a second place for a typo; every stream
// needs it from its first day, and none could otherwise build input data
// before the stream providing it had landed; and determinism across the whole
// tool depends on every component calling the SAME function, which is a
// property of one definition, not of two that agree on a test set.
//
// The definition's own tests are the published vectors, read from a fixture
// copied verbatim from Random123's tests/kat_vectors (F21: an external model's
// answers come from the external tool, not from memory): tests/contracts/rng/.
//
// DETERMINISM: every function here is a pure function of its arguments. No
// state, no thread-local, no dependence on thread count, decomposition, ISA
// path or architecture: integer arithmetic only, unsigned throughout, so the
// BitExact class of docs/determinism.md §2 holds for anything built on it.
// THREAD-SAFETY: every function is safe to call from any thread at any time.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::rng {

/// One Philox4x32 counter, or one block of output: four 32-bit words.
using Block = std::array<std::uint32_t, 4>;
/// One Philox4x32 key: two 32-bit words.
using Key = std::array<std::uint32_t, 2>;

/// The published constants (Salmon et al., Table 2; Random123 philox.h).
inline constexpr std::uint32_t kPhiloxM0 = 0xD2511F53U;  ///< multiplier, words 0 and 1
inline constexpr std::uint32_t kPhiloxM1 = 0xCD9E8D57U;  ///< multiplier, words 2 and 3
inline constexpr std::uint32_t kPhiloxW0 = 0x9E3779B9U;  ///< key bump, word 0 (golden ratio)
inline constexpr std::uint32_t kPhiloxW1 = 0xBB67AE85U;  ///< key bump, word 1 (sqrt(3) - 1)
inline constexpr int kPhiloxRounds = 10;
/// Bytes in one Block, serialised little-endian word by word.
inline constexpr std::size_t kBlockBytes = 16;

/// Philox4x32-10: the block for `counter` under `key`.
///
/// Each round, with hi/lo the high and low 32 bits of a 64-bit product:
///   p0 = M0 * c0, p1 = M1 * c2
///   c' = { hi(p1) ^ c1 ^ k0, lo(p1), hi(p0) ^ c3 ^ k1, lo(p0) }
/// and between rounds the key is bumped by (W0, W1), modulo 2^32. Ten rounds,
/// nine bumps. Equal to Random123's philox4x32_R(10, ctr, key) word for word.
[[nodiscard]] constexpr Block philox4x32_10(Block counter, Key key) noexcept {
  for (int round = 0; round < kPhiloxRounds; ++round) {
    if (round > 0) {
      key[0] += kPhiloxW0;
      key[1] += kPhiloxW1;
    }
    const std::uint64_t product0 = std::uint64_t{kPhiloxM0} * counter[0];
    const std::uint64_t product1 = std::uint64_t{kPhiloxM1} * counter[2];
    counter = Block{static_cast<std::uint32_t>(product1 >> 32U) ^ counter[1] ^ key[0],
                    static_cast<std::uint32_t>(product1),
                    static_cast<std::uint32_t>(product0 >> 32U) ^ counter[3] ^ key[1],
                    static_cast<std::uint32_t>(product0)};
  }
  return counter;
}

/// The key a run's seed selects: the low word first.
[[nodiscard]] constexpr Key key_of(std::uint64_t seed) noexcept {
  return Key{static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32U)};
}

/// The project's addressing convention, which every component uses so that one
/// (seed, stream, position) names the same bits everywhere:
///
///   counter = { position low, position high, stream low, stream high }
///   key     = key_of(seed)
///
/// A STREAM is an independent sequence -- by convention a workload unit's index
/// (workloads/workload.hpp), so unit i's input is stream i and no two units
/// share data. POSITION counts 16-byte blocks from the start of the stream.
[[nodiscard]] constexpr Block block_at(std::uint64_t seed, std::uint64_t stream,
                                       std::uint64_t position) noexcept {
  return philox4x32_10(
      Block{static_cast<std::uint32_t>(position), static_cast<std::uint32_t>(position >> 32U),
            static_cast<std::uint32_t>(stream), static_cast<std::uint32_t>(stream >> 32U)},
      key_of(seed));
}

/// Fills `out` with the first out.size() bytes of stream `stream`: byte k is
/// byte (k mod 16) of block_at(seed, stream, k / 16), each block serialised as
/// its four words in order, each word little-endian -- on every architecture,
/// so the bytes do not depend on the machine's byte order. An empty span is
/// filled with nothing. The result for a prefix is a prefix of the result.
///
/// Not named fill: a call fill(seed, stream, vector) finds std::fill by
/// argument-dependent lookup and prefers it, which is how this was learned.
constexpr void fill_stream(std::uint64_t seed, std::uint64_t stream,
                           std::span<std::byte> out) noexcept {
  for (std::size_t offset = 0; offset < out.size(); offset += kBlockBytes) {
    const Block block = block_at(seed, stream, offset / kBlockBytes);
    for (std::size_t byte = 0; byte < kBlockBytes && offset + byte < out.size(); ++byte) {
      const std::uint32_t word = block.at(byte / 4);
      out[offset + byte] = static_cast<std::byte>((word >> (8U * (byte % 4))) & 0xFFU);
    }
  }
}

/// The 64-bit value a block's first two words make, word 0 low. For workloads
/// that draw integers: value j of stream s is u64_of(block_at(seed, s, j)).
[[nodiscard]] constexpr std::uint64_t u64_of(const Block& block) noexcept {
  return std::uint64_t{block[0]} | (std::uint64_t{block[1]} << 32U);
}

/// A double in [0, 1) from 64 random bits: the top 53 bits, scaled by 2^-53.
/// Exact -- every result is a multiple of 2^-53, so no rounding happens and
/// the value is the same on every IEEE 754 machine.
[[nodiscard]] constexpr double unit_interval(std::uint64_t bits) noexcept {
  constexpr double kTwoToMinus53 = 1.0 / 9007199254740992.0;
  return static_cast<double>(bits >> 11U) * kTwoToMinus53;
}

}  // namespace loadforge::rng

#endif
