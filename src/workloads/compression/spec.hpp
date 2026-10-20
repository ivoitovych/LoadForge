// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_COMPRESSION_SPEC_HPP
#define LOADFORGE_WORKLOADS_COMPRESSION_SPEC_HPP

// CONTRACT (ADR 0004). The specification the compression workload and its
// independent oracle are both written from (verification.md §3, PLAN F14).
// Frozen: changed only by a contract change, never inside a stream.
//
//   The compression stream writes the stress implementation (the
//     compressor, src/workloads/compression/).
//   The exact-oracle stream writes the oracle (an independent decoder,
//     src/verification/oracle/compression/), from this file and the LZ4
//     block-format document -- never from the compressor.
//
// WHY LZ4'S BLOCK FORMAT
// ----------------------
// The two implementations need a format neither of them invented, so that
// each can be checked against a third party: the LZ4 block format is
// published (lz4/doc/lz4_Block_format.md in the lz4 project), small enough
// to decode in a page, and has a reference tool -- the lz4 command line, whose
// legacy frame (lz4 -l) is a sequence of raw blocks -- to make fixtures from
// (F21). The compressor's choices of matches are its own; any valid block
// that decodes to the input is a right answer, which is what makes the
// oracle a decoder and not a second compressor.
//
// A UNIT
// ------
//   input   kInputBytes bytes, from the workload's generator over Philox
//           stream `index` (primitives/rng/philox.hpp). The generator is the
//           workload's, and must give data that compresses to between a fifth
//           and four fifths of its size, so literals and matches are both
//           exercised (a T4 property the workload tests).
//   output  output_bytes_for(kInputBytes) bytes:
//             [0, 4)          kMagic, little-endian
//             [4, 8)          L, the block's length, little-endian u32
//             [8, 8 + L)      one LZ4 block that decodes to the input
//             [8 + L, end)    zero
//
// DETERMINISM: BitExact. One unit is compressed by one thread; the output is
// a function of the input alone. The workload has no decomposition: its plan
// reports 1, and any other configured value is refused as kConfiguration.
//
// WHAT THE ORACLE ANSWERS
// -----------------------
// Each is a verdict on the unit's output given its input (oracle.hpp):
//   * the magic, L <= compress_bound(kInputBytes), the zero padding, every
//     token, offset and length, and the end-of-block rules (the last five
//     bytes are literals; the last match starts at least twelve bytes before
//     the end) are checked as the format document states them. Any breach is
//     an ExactMismatch with no difference, its check naming what was wrong
//     and at which byte of the output: the output is wrong, and has no bits
//     to compare.
//   * a block that decodes to anything but kInputBytes bytes is the same.
//   * a block that decodes to kInputBytes bytes is compared with the input:
//     a difference is an ExactMismatch with a Difference in Domain::kDerived
//     (the decoded bytes), and none is an Agreement with blocks_compared 1.
//   * evidence of the wrong size is NotChecked.

#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::workloads::compression {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kInputBytes = std::size_t{64} << 10U;
inline constexpr std::uint32_t kMagic = 0x315A464CU;  // "LFZ1"
inline constexpr std::size_t kHeaderBytes = 8;

/// LZ4_COMPRESSBOUND from lz4.h: the largest block a compressor may emit for
/// n input bytes.
[[nodiscard]] constexpr std::size_t compress_bound(std::size_t n) noexcept {
  return n + n / 255 + 16;
}

[[nodiscard]] constexpr std::size_t output_bytes_for(std::size_t input_bytes) noexcept {
  return kHeaderBytes + compress_bound(input_bytes);
}

/// The little-endian u32 at `at`. Precondition: at + 4 <= bytes.size().
[[nodiscard]] constexpr std::uint32_t load_le32(std::span<const std::byte> bytes,
                                                std::size_t at) noexcept {
  return std::to_integer<std::uint32_t>(bytes[at]) |
         (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[at + 2]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[at + 3]) << 24U);
}

}  // namespace loadforge::workloads::compression

#endif
