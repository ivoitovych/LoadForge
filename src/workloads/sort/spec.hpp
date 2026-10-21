// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_SORT_SPEC_HPP
#define LOADFORGE_WORKLOADS_SORT_SPEC_HPP

// CONTRACT (ADR 0004). The specification the sort workload and its
// independent oracle are both written from (verification.md §3, §4a;
// determinism.md §2; IMPLEMENTATION M4). Frozen: changed only by a contract
// change, never inside a stream.
//
//   The sort stream writes the stress implementation: a parallel radix or
//     merge sort (src/workloads/sort/).
//   The wave-2 oracle stream writes the oracle (src/verification/oracle/sort/)
//     from this file -- never from the sort. It checks the output; it never
//     sorts anything (verification.md §3: "never a second sort").
//
// WHY THE OUTPUT CARRIES EACH KEY'S SOURCE POSITION
// -------------------------------------------------
// A sorted array of keys alone can only be shown to be a permutation of the
// input by sorting the input (forbidden), by counting with binary searches
// (O(n log n) and a scratch array, and still no expected bits), or by a
// multiset hash (probabilistic, and no expected bits either). So the output
// carries a witness: beside each key, the input position it came from. With
// it the check is exact, linear and needs no sort -- the certifying-algorithm
// pattern (McConnell, Mehlhorn, Naeher and Schweitzer, "Certifying
// algorithms", Computer Science Review 5(2), 2011) -- and a key that is wrong
// has a known expected value, the input key its index names, so a flipped bit
// is reported AS a flipped bit. Ties are broken by that position (a stable
// sort), which makes the output a function of the input alone, whatever the
// algorithm.
//
// A UNIT
// ------
//   input   kKeys keys, each a little-endian u64: kInputBytes bytes. Key e
//           (e = 0 .. kKeys - 1, the key's INPUT POSITION) is
//           key_at(seed, index, e) = u64_of(block_at(seed, index, e)): any
//           value in [0, 2^64 - 1], every one of which can occur. Keys are
//           compared as unsigned 64-bit integers.
//   output  kOutputBytes bytes, two regions, for output positions
//           j = 0 .. kKeys - 1:
//             [key_offset(j), +8)     K_j, a key, little-endian u64
//             [index_offset(j), +4)   I_j, the input position K_j came from,
//                                     little-endian u32
//           The keys' region is [0, kIndexRegionOffset); the indices' region
//           is [kIndexRegionOffset, kOutputBytes). No padding.
//   The right output is the unique one in which the I_j are a permutation of
//   0 .. kKeys - 1, each K_j is input key I_j (the u64 at input bytes
//   [8 I_j, 8 I_j + 8)), and the pairs strictly increase:
//   precedes((K_{j-1}, I_{j-1}), (K_j, I_j)) for every j >= 1 -- keys in
//   non-decreasing order, equal keys in increasing input position. compute()
//   is total (workload.hpp): it produces exactly this for ANY input bytes,
//   including inputs with equal keys, which the generator makes with
//   probability below 2^-30 per unit and a corrupted input can make at will.
//
// DECOMPOSITION: the chunk count P, one of kChunkCounts, default
// kDefaultChunkCount. The implementation splits a unit's input positions into
// P contiguous ranges of kKeys / P positions each, range r being
// [r kKeys / P, (r + 1) kKeys / P), and spreads its work over the executor by
// range -- per-range histograms for a radix sort, per-range runs for a merge
// sort -- never by thread count. A configured decomposition of 0 resolves to
// kDefaultChunkCount; a value in kChunkCounts resolves to itself; every
// other value (neither 0 nor in kChunkCounts) is refused as kConfiguration by
// the workload and by the oracle. The plan reports the resolved P, and the
// oracle also refuses, as kConfiguration, a plan whose decomposition is not in
// kChunkCounts or whose input_bytes or output_bytes is not kInputBytes or
// kOutputBytes.
//
// DETERMINISM: kBitExact. The output is a function of the input alone, so it
// is the same bits at any thread count, on any run, for every P, on every ISA
// path and on x86-64 and ARM64 (determinism.md §2). Unsigned integer
// operations only. Contract: kExact.
//
// WHAT THE ORACLE CHECKS, AND IN WHICH ORDER
// ------------------------------------------
// Evidence whose input is not kInputBytes or whose output is not kOutputBytes
// bytes is NotChecked, blocks_compared 0. Otherwise three steps, in this
// order; the first step that fails gives the verdict, and the later ones are
// not run:
//
//   1. INDICES. For j = 0, 1, ...: I_j >= kKeys, or I_j equal to some I_i with
//      i < j, fails at j. The verdict names the smallest such j: an
//      ExactMismatch with no difference, check "index out of range at
//      position <j>" or "index repeated at position <j>" (j in decimal),
//      blocks_compared j + 1 -- the positions read. The oracle may need
//      kKeys bits of memory to see a repeat; if it cannot obtain what it
//      needs, the verdict is NotChecked, blocks_compared 0, whatever the
//      output holds.
//   2. KEYS. Every K_j is compared with input key I_j (the indices are now a
//      proven permutation, so this is a comparison with a known expected
//      value). If any differ: an ExactMismatch, check "key differs from input
//      key <I_j> at position <j>" for the smallest such j, and a Difference
//      in Domain::kStored over the keys' region -- offsets from the start of
//      the output, compared length kIndexRegionOffset, expected bytes the
//      input keys the indices name (byte o of the region is byte o mod 8 of
//      input key I_{o / 8}), actual bytes the output's -- popcount over the
//      whole region, window as record.hpp defines it. blocks_compared kKeys.
//   3. ORDER. For j = 1, 2, ...: precedes((K_{j-1}, I_{j-1}), (K_j, I_j))
//      false fails at j. The smallest such j is named: an ExactMismatch with
//      no difference (a misordered permutation has no expected bits without
//      the sort the oracle may not do), check "order broken at position
//      <j>", blocks_compared kKeys.
//
// None failing: an Agreement, blocks_compared kKeys. Steps 1 to 3 passing
// prove the output right: the indices are a permutation, each key is the
// input key it names, so the keys are the input permuted, and in the one
// order the pairs allow. Consequences each side's fault-injection tests can
// rely on: one flipped bit in a key is a step-2 mismatch whose difference has
// popcount 1; one flipped bit in an index makes it out of range or a repeat
// (step 1, no difference); input corrupted before compute() sorts correctly
// and is an Agreement, and input corrupted after compute() but before the
// check reads as a step-2 mismatch -- in both, the input-integrity bracket's
// checks are what say the input was wrong (verification.md §4a, oracle.hpp).
//
// VERSION: kVersion is the workload's Descriptor::version. It is raised by a
// contract change that alters the bytes any configuration produces -- the
// generator, the sizes, the layout or the order -- and by nothing else; a
// golden vector or error record names the version it was made under, and is
// never compared across versions. A change only to what the oracle reports
// (a check's wording) leaves it alone.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "primitives/rng/philox.hpp"

namespace loadforge::workloads::sort {

inline constexpr std::string_view kName = "sort";
inline constexpr std::uint32_t kVersion = 1;

inline constexpr std::size_t kKeys = std::size_t{1} << 17U;
inline constexpr std::size_t kKeyBytes = 8;
inline constexpr std::size_t kIndexBytes = 4;
inline constexpr std::size_t kInputBytes = kKeys * kKeyBytes;
inline constexpr std::size_t kIndexRegionOffset = kKeys * kKeyBytes;
inline constexpr std::size_t kOutputBytes = kKeys * (kKeyBytes + kIndexBytes);

inline constexpr std::array<std::uint32_t, 7> kChunkCounts = {1, 2, 4, 8, 16, 32, 64};
inline constexpr std::uint32_t kDefaultChunkCount = 16;

/// Whether a resolved decomposition is one the specification has. 0 is not:
/// it is a request for the default, which resolves to kDefaultChunkCount.
[[nodiscard]] constexpr bool is_chunk_count(std::uint32_t chunks) noexcept {
  return std::ranges::any_of(kChunkCounts,
                             [chunks](std::uint32_t count) { return count == chunks; });
}

/// The key at input position e of unit `index` under `seed`.
[[nodiscard]] constexpr std::uint64_t key_at(std::uint64_t seed, std::uint64_t index,
                                             std::uint64_t position) noexcept {
  return rng::u64_of(rng::block_at(seed, index, position));
}

/// Where output position j's key and index start, from the start of the output.
[[nodiscard]] constexpr std::size_t key_offset(std::size_t position) noexcept {
  return position * kKeyBytes;
}
[[nodiscard]] constexpr std::size_t index_offset(std::size_t position) noexcept {
  return kIndexRegionOffset + position * kIndexBytes;
}

/// The little-endian u64 at `at`. Precondition: at + 8 <= bytes.size().
[[nodiscard]] constexpr std::uint64_t load_le64(std::span<const std::byte> bytes,
                                                std::size_t at) noexcept {
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < kKeyBytes; ++byte) {
    value |= std::to_integer<std::uint64_t>(bytes[at + byte]) << (8U * byte);
  }
  return value;
}

/// The little-endian u32 at `at`. Precondition: at + 4 <= bytes.size().
[[nodiscard]] constexpr std::uint32_t load_le32(std::span<const std::byte> bytes,
                                                std::size_t at) noexcept {
  std::uint32_t value = 0;
  for (std::size_t byte = 0; byte < kIndexBytes; ++byte) {
    value |= std::to_integer<std::uint32_t>(bytes[at + byte]) << (8U * byte);
  }
  return value;
}

/// One output position: a key and the input position it came from.
struct Entry {
  std::uint64_t key = 0;
  std::uint32_t index = 0;
};

/// The order of the output: by key as an unsigned integer, then by index.
/// Strict -- an entry does not precede itself.
[[nodiscard]] constexpr bool precedes(const Entry& lhs, const Entry& rhs) noexcept {
  return lhs.key < rhs.key || (lhs.key == rhs.key && lhs.index < rhs.index);
}

}  // namespace loadforge::workloads::sort

#endif
