// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_COMPRESSION_LZ4_BLOCK_HPP
#define LOADFORGE_WORKLOADS_COMPRESSION_LZ4_BLOCK_HPP

// The stress implementation's compressor: one LZ4 block, as the block-format
// document (lz4/doc/lz4_Block_format.md) defines it, from input of any length.
//
// WHAT IT CHOOSES, AND WHAT IT MAY NOT
// ------------------------------------
// The format fixes how a block is read, not which matches a compressor picks
// (spec.hpp: any valid block that decodes to the input is a right answer).
// This one is greedy over a single-entry hash table, the shape of the
// reference implementation's fast mode without its acceleration: at each
// position the four bytes there are hashed, the last position that hashed the
// same is the only candidate, and a candidate whose four bytes really match is
// extended forwards as far as the end-of-block rules allow. No backward
// extension, no skipping ahead on incompressible data: both would make the
// output depend on more than the next position's bytes, and neither is needed
// for a block that decodes.
//
// The end-of-block rules are kept by construction rather than checked after:
//   * no match starts later than kMatchFindLimit bytes before the end, so the
//     last match starts at least twelve bytes before it;
//   * no match extends into the last kLastLiterals bytes, so they are always
//     literals, in a last sequence of literals only;
//   * an input shorter than kMatchFindLimit + 1 bytes is one literal-only
//     sequence -- the format's "blocks < 13 bytes cannot be compressed".
//
// THE BOUND
// ---------
// The block never exceeds compress_bound(input.size()). Each sequence with a
// match costs its token, two offset bytes and its length bytes, and covers at
// least kMinMatch more input bytes than literals; a match's own extra length
// bytes are fewer than the bytes it covers. What is left is one extra byte
// per 255 literals in a run and the final token, which is n / 255 + 16 with
// room to spare. tests/property/workloads/compression/ holds the inputs that
// come closest.
//
// DETERMINISM: the block is a function of the input bytes alone. The hash is
// computed from little-endian loads (spec.hpp, load_le32), so the candidates
// and therefore the block are the same on every architecture, and the table
// is cleared on every call, so nothing a previous call left in it survives.
//
// TOTALITY: any input bytes are input. Every read is inside `input`, every
// write inside the first compress_bound(input.size()) bytes of `block`, and
// every candidate the table holds is a position this call wrote there, below
// the current one.

#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::workloads::compression {

/// The format's constants (lz4_Block_format.md): the shortest match, the
/// literals that always end a block, and how far before the end the last
/// match may start (the reference implementation's MFLIMIT).
inline constexpr std::size_t kMinMatch = 4;
inline constexpr std::size_t kLastLiterals = 5;
inline constexpr std::size_t kMatchFindLimit = 12;
/// The largest offset two bytes can code.
inline constexpr std::size_t kMaxOffset = 65535;
/// A length field's nibble value that says more bytes follow.
inline constexpr std::size_t kLengthEscape = 15;

/// The match finder's table: 2^kHashLog positions, four bytes each.
inline constexpr unsigned kHashLog = 12;
inline constexpr std::size_t kHashEntries = std::size_t{1} << kHashLog;
inline constexpr std::size_t kTableBytes = kHashEntries * 4;

/// Compresses `input` into one LZ4 block written to the front of `block`, and
/// returns the block's length. `table` is the match finder's working memory;
/// what it held before does not matter, and what it holds after is not part
/// of the result.
///
/// Preconditions: block.size() >= compress_bound(input.size()),
/// table.size() >= kTableBytes, and input.size() < 2^32.
[[nodiscard]] std::size_t compress_block(std::span<const std::byte> input,
                                         std::span<std::byte> block,
                                         std::span<std::byte> table) noexcept;

/// One sequence written at `at` in `block`: the token, the literal length's
/// extra bytes, the literals, and -- unless `match_length` is 0, which makes
/// this the literal-only last sequence -- the offset and the match length's
/// extra bytes. Returns the position after it.
///
/// Preconditions: match_length is 0 or at least kMinMatch, offset is in
/// [1, kMaxOffset] when match_length is not 0, and the sequence fits.
[[nodiscard]] std::size_t write_sequence(std::span<std::byte> block, std::size_t at,
                                         std::span<const std::byte> literals, std::size_t offset,
                                         std::size_t match_length) noexcept;

/// The table slot four bytes hash to: Knuth's multiplicative hash, the
/// product's top kHashLog bits.
[[nodiscard]] constexpr std::size_t hash_slot(std::uint32_t four_bytes) noexcept {
  return static_cast<std::size_t>((four_bytes * 2654435761U) >> (32U - kHashLog));
}

}  // namespace loadforge::workloads::compression

#endif
